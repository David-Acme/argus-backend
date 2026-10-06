#include "identity-sync-gateway.hxx"

#include <auth/module-gate.hxx>
#include <errors/response-exception.hxx>
#include <grpc/grpc-client-base.hxx>
#include <sync/sync-errors.hxx>
#include <json/value.h>
#include <sync/sync-filter.hxx>
#include <runtime/blocking-task.hxx>
#include <cstdint>
#include <string>
#include <auth/user-role.hxx>
#include <utility>
#include <vector>

namespace
{
argus::client::CallerIdentity identityFor(const JwtContext& ctx)
{
  return {.userId = ctx.sub,
          .role = userRoleToString(ctx.role),
          .device = ctx.deviceHash,
          .roleActive = moduleGate().roleActive(ctx.role)};
}

argus::identity::v1::SyncRange toRange(const SyncFilter& filter)
{
  argus::identity::v1::SyncRange range;
  if (filter.startTime)
    range.set_start_time(*filter.startTime);
  if (filter.startId)
    range.set_start_id(*filter.startId);
  if (filter.endTime)
    range.set_end_time(*filter.endTime);
  return range;
}

argus::identity::v1::TablePull*
mutablePull(argus::identity::v1::PullTableRequest& request)
{
  switch (request.table_case()) {
    case argus::identity::v1::PullTableRequest::kUser:
      return request.mutable_user();
    case argus::identity::v1::PullTableRequest::kUserInvitation:
      return request.mutable_user_invitation();
    case argus::identity::v1::PullTableRequest::kPerson:
      return request.mutable_person();
    default:
      return nullptr;
  }
}

Json::Value userRowToJson(const argus::identity::v1::UserRow& row)
{
  Json::Value json(Json::objectValue);
  json["id"] = static_cast<Json::Int64>(row.id());
  json["name"] = row.name();
  json["lastName"] = row.last_name();
  json["role"] = row.role();
  json["lang"] = row.lang();
  json["isActive"] = row.is_active();
  json["createdAt"] = static_cast<Json::Int64>(row.created_at());
  json["updatedAt"] = row.has_updated_at()
                          ? Json::Value(static_cast<Json::Int64>(row.updated_at()))
                          : Json::Value();
  json["syncAt"] = static_cast<Json::Int64>(row.sync_at());
  json["deletedAt"] = row.has_deleted_at()
                          ? Json::Value(static_cast<Json::Int64>(row.deleted_at()))
                          : Json::Value();
  return json;
}

Json::Value
invitationRowToJson(const argus::identity::v1::UserInvitationRow& row)
{
  Json::Value json(Json::objectValue);
  json["id"] = static_cast<Json::Int64>(row.id());
  json["role"] = row.role();
  json["maxRedemptions"] = row.max_redemptions();
  json["redemptionCount"] = row.redemption_count();
  json["expiresAt"] = static_cast<Json::Int64>(row.expires_at());
  json["createdBy"] = static_cast<Json::Int64>(row.created_by());
  json["revokedAt"] = row.has_revoked_at()
                          ? Json::Value(static_cast<Json::Int64>(row.revoked_at()))
                          : Json::Value();
  json["createdAt"] = static_cast<Json::Int64>(row.created_at());
  json["updatedAt"] = row.has_updated_at()
                          ? Json::Value(static_cast<Json::Int64>(row.updated_at()))
                          : Json::Value();
  json["syncAt"] = static_cast<Json::Int64>(row.sync_at());
  return json;
}

Json::Value personRowToJson(const argus::identity::v1::PersonRow& row)
{
  Json::Value json(Json::objectValue);
  json["id"] = static_cast<Json::Int64>(row.id());
  json["userId"] = row.has_user_id()
                       ? Json::Value(static_cast<Json::Int64>(row.user_id()))
                       : Json::Value();
  json["name"] = row.name();
  json["alias"] = row.alias();
  json["observation"] = row.observation();
  json["status"] = row.status();
  json["firstSeenAt"] = static_cast<Json::Int64>(row.first_seen_at());
  json["lastSeenAt"] = static_cast<Json::Int64>(row.last_seen_at());
  json["createdAt"] = static_cast<Json::Int64>(row.created_at());
  json["updatedAt"] = row.has_updated_at()
                          ? Json::Value(static_cast<Json::Int64>(row.updated_at()))
                          : Json::Value();
  json["deletedAt"] = row.has_deleted_at()
                          ? Json::Value(static_cast<Json::Int64>(row.deleted_at()))
                          : Json::Value();
  return json;
}

Json::Value deletedRowToJson(const argus::identity::v1::DeletedRow& row)
{
  Json::Value json(Json::objectValue);
  json["id"] = static_cast<Json::Int64>(row.id());
  json["deletedAt"] = static_cast<Json::Int64>(row.deleted_at());
  return json;
}

ResponseException unavailable()
{
  return {503, SyncErrors::IdentitySyncUnavailable};
}
}

class IdentitySyncGateway::Pull : public Syncable
{
public:
  struct Deps
  {
    std::shared_ptr<IdentitySyncClient> client;
    IdentitySyncTable table{IdentitySyncTable::User};
    argus::client::CallerIdentity identity;
  };

  explicit Pull(const Deps& deps)
      : client_(deps.client), table_(deps.table), identity_(deps.identity)
  {
  }

  drogon::Task<std::vector<Json::Value>>
  find(const SyncFilter& filter) const override
  {
    const auto response =
        co_await pull({.table = table_, .mode = Mode::Created, .filter = filter});
    co_return createdRows(response);
  }

  drogon::Task<std::vector<Json::Value>>
  findDeleted(const SyncFilter& filter) const override
  {
    const auto response =
        co_await pull({.table = table_, .mode = Mode::Deleted, .filter = filter});
    co_return deletedRows(response);
  }

  drogon::Task<std::optional<Json::Value>>
  findLast(const SyncFilter&) const override
  {
    const auto response = co_await pull(
        {.table = table_, .mode = Mode::LastCreated, .filter = {}});
    std::optional<Json::Value> row;
    switch (response.table_case()) {
      case argus::identity::v1::PullTableResponse::kUser:
        if (response.user().has_last_created())
          row = userRowToJson(response.user().last_created());
        break;
      case argus::identity::v1::PullTableResponse::kUserInvitation:
        if (response.user_invitation().has_last_created())
          row = invitationRowToJson(
              response.user_invitation().last_created());
        break;
      case argus::identity::v1::PullTableResponse::kPerson:
        if (response.person().has_last_created())
          row = personRowToJson(response.person().last_created());
        break;
      default:
        throw unavailable();
    }
    co_return row;
  }

  drogon::Task<std::optional<Json::Value>>
  findLastDeleted(const SyncFilter&) const override
  {
    const auto response = co_await pull(
        {.table = table_, .mode = Mode::LastDeleted, .filter = {}});
    std::optional<Json::Value> row;
    switch (response.table_case()) {
      case argus::identity::v1::PullTableResponse::kUser:
        if (response.user().has_last_deleted())
          row = deletedRowToJson(response.user().last_deleted());
        break;
      case argus::identity::v1::PullTableResponse::kUserInvitation:
        if (response.user_invitation().has_last_deleted())
          row = deletedRowToJson(response.user_invitation().last_deleted());
        break;
      case argus::identity::v1::PullTableResponse::kPerson:
        if (response.person().has_last_deleted())
          row = deletedRowToJson(response.person().last_deleted());
        break;
      default:
        throw unavailable();
    }
    co_return row;
  }

private:
  enum class Mode : std::uint8_t
  {
    Created,
    Deleted,
    LastCreated,
    LastDeleted,
  };

  struct RequestForInput
  {
    IdentitySyncTable table{IdentitySyncTable::User};
    Mode mode{Mode::Created};
    const SyncFilter& filter;
  };

  static argus::identity::v1::PullTableRequest
  requestFor(const RequestForInput& input)
  {
    const IdentitySyncTable table = input.table;
    const Mode mode = input.mode;
    const SyncFilter& filter = input.filter;

    argus::identity::v1::PullTableRequest request;
    switch (table) {
      case IdentitySyncTable::User:
        request.mutable_user();
        break;
      case IdentitySyncTable::UserInvitation:
        request.mutable_user_invitation();
        break;
      case IdentitySyncTable::Person:
        request.mutable_person();
        break;
    }
    auto* body = mutablePull(request);
    switch (mode) {
      case Mode::Created:
        body->set_required_create(true);
        *body->mutable_created() = toRange(filter);
        break;
      case Mode::Deleted:
        body->set_required_deleted(true);
        *body->mutable_deleted() = toRange(filter);
        break;
      case Mode::LastCreated:
        body->set_find_last_created(true);
        break;
      case Mode::LastDeleted:
        body->set_find_last_deleted(true);
        break;
    }
    return request;
  }

  struct PullInput
  {
    IdentitySyncTable table{IdentitySyncTable::User};
    Mode mode{Mode::Created};
    const SyncFilter& filter;
  };

  drogon::Task<argus::identity::v1::PullTableResponse>
  pull(const PullInput& input) const
  {
    const auto request = requestFor({.table = input.table,
                                     .mode = input.mode,
                                     .filter = input.filter});
    auto response = co_await BlockingTask<
        std::optional<argus::identity::v1::PullTableResponse>>(
        [this, request]() { return client_->pullTable(request, identity_); });
    if (!response)
      throw unavailable();
    co_return std::move(*response);
  }

  static std::vector<Json::Value>
  createdRows(const argus::identity::v1::PullTableResponse& response)
  {
    std::vector<Json::Value> rows;
    switch (response.table_case()) {
      case argus::identity::v1::PullTableResponse::kUser:
        for (const auto& row : response.user().created())
          rows.push_back(userRowToJson(row));
        break;
      case argus::identity::v1::PullTableResponse::kUserInvitation:
        for (const auto& row : response.user_invitation().created())
          rows.push_back(invitationRowToJson(row));
        break;
      case argus::identity::v1::PullTableResponse::kPerson:
        for (const auto& row : response.person().created())
          rows.push_back(personRowToJson(row));
        break;
      default:
        throw unavailable();
    }
    return rows;
  }

  static std::vector<Json::Value>
  deletedRows(const argus::identity::v1::PullTableResponse& response)
  {
    std::vector<Json::Value> rows;
    switch (response.table_case()) {
      case argus::identity::v1::PullTableResponse::kUser:
        for (const auto& row : response.user().deleted())
          rows.push_back(deletedRowToJson(row));
        break;
      case argus::identity::v1::PullTableResponse::kUserInvitation:
        for (const auto& row : response.user_invitation().deleted())
          rows.push_back(deletedRowToJson(row));
        break;
      case argus::identity::v1::PullTableResponse::kPerson:
        for (const auto& row : response.person().deleted())
          rows.push_back(deletedRowToJson(row));
        break;
      default:
        throw unavailable();
    }
    return rows;
  }

  std::shared_ptr<IdentitySyncClient> client_;
  IdentitySyncTable table_;
  argus::client::CallerIdentity identity_;
};

IdentitySyncGateway::IdentitySyncGateway(IdentitySyncClientConfig config)
    : client_(std::make_shared<IdentitySyncClient>(std::move(config)))
{
}

bool IdentitySyncGateway::serves(IdentitySyncTable) const
{
  return true;
}

std::unique_ptr<Syncable>
IdentitySyncGateway::sourceFor(IdentitySyncTable table,
                               const JwtContext& ctx) const
{
  return std::make_unique<Pull>(
      Pull::Deps{.client = client_, .table = table, .identity = identityFor(ctx)});
}
