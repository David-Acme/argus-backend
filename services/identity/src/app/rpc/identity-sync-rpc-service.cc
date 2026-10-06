#include "identity-sync-rpc-service.hxx"
#include "identity-callers.hxx"

#include <auth/role-access.hxx>
#include <auth/user-role.hxx>
#include <drogon/drogon.h>
#include <grpc/grpc-server-identity.hxx>
#include <optional>
#include <sync/role-permission.hxx>
#include <sync/sync-filter.hxx>
#include <sync/table-name.hxx>
#include <trantor/utils/Logger.h>

namespace
{

SyncFilter filterOf(const argus::identity::v1::SyncRange& range)
{
  SyncFilter filter;
  if (range.has_start_time())
    filter.startTime = range.start_time();
  if (range.has_start_id())
    filter.startId = range.start_id();
  if (range.has_end_time())
    filter.endTime = range.end_time();
  return filter;
}

SyncFilter scoped(const argus::identity::v1::SyncRange& range,
                  const SyncFilter& scope)
{
  SyncFilter filter = filterOf(range);
  filter.userId = scope.userId;
  return filter;
}

std::optional<TableName>
tableOf(const argus::identity::v1::PullTableRequest& request)
{
  switch (request.table_case()) {
    case argus::identity::v1::PullTableRequest::kUser:
      return TableName::User;
    case argus::identity::v1::PullTableRequest::kUserInvitation:
      return TableName::UserInvitation;
    case argus::identity::v1::PullTableRequest::kPerson:
      return TableName::Person;
    default:
      return std::nullopt;
  }
}

void toProto(const Json::Value& row, argus::identity::v1::UserRow* out)
{
  out->set_id(row["id"].asInt64());
  out->set_name(row["name"].asString());
  out->set_last_name(row["lastName"].asString());
  out->set_role(row["role"].asString());
  out->set_lang(row["lang"].asString());
  out->set_is_active(row["isActive"].asBool());
  out->set_created_at(row["createdAt"].asInt64());
  if (!row["updatedAt"].isNull())
    out->set_updated_at(row["updatedAt"].asInt64());
  out->set_sync_at(row["syncAt"].asInt64());
  if (!row["deletedAt"].isNull())
    out->set_deleted_at(row["deletedAt"].asInt64());
}

void toProto(const Json::Value& row, argus::identity::v1::UserInvitationRow* out)
{
  out->set_id(row["id"].asInt64());
  out->set_role(row["role"].asString());
  out->set_max_redemptions(row["maxRedemptions"].asInt());
  out->set_redemption_count(row["redemptionCount"].asInt());
  out->set_expires_at(row["expiresAt"].asInt64());
  out->set_created_by(row["createdBy"].asInt64());
  if (!row["revokedAt"].isNull())
    out->set_revoked_at(row["revokedAt"].asInt64());
  out->set_created_at(row["createdAt"].asInt64());
  if (!row["updatedAt"].isNull())
    out->set_updated_at(row["updatedAt"].asInt64());
  out->set_sync_at(row["syncAt"].asInt64());
}

void toProto(const Json::Value& row, argus::identity::v1::PersonRow* out)
{
  out->set_id(row["id"].asInt64());
  if (!row["userId"].isNull())
    out->set_user_id(row["userId"].asInt64());
  out->set_name(row["name"].asString());
  out->set_alias(row["alias"].asString());
  out->set_observation(row["observation"].asString());
  out->set_status(row["status"].asString());
  out->set_first_seen_at(row["firstSeenAt"].asInt64());
  out->set_last_seen_at(row["lastSeenAt"].asInt64());
  out->set_created_at(row["createdAt"].asInt64());
  if (!row["updatedAt"].isNull())
    out->set_updated_at(row["updatedAt"].asInt64());
  if (!row["deletedAt"].isNull())
    out->set_deleted_at(row["deletedAt"].asInt64());
}

template <typename TableRows, typename Repo>
struct FillInput
{
  const argus::identity::v1::TablePull& body;
  TableRows* rows;
  const Repo& repo;
  const SyncFilter& scope;
};

template <typename TableRows, typename Repo>
drogon::Task<void> fill(const FillInput<TableRows, Repo>& input)
{
  const argus::identity::v1::TablePull& body = input.body;
  TableRows* rows = input.rows;
  const Repo& repo = input.repo;
  const SyncFilter& scope = input.scope;
  if (body.required_create()) {
    const auto data = co_await repo.find(scoped(body.created(), scope));
    for (const auto& row : data)
      toProto(row, rows->add_created());
  }
  if (body.required_deleted()) {
    const auto data = co_await repo.findDeleted(scoped(body.deleted(), scope));
    for (const auto& row : data) {
      auto* tombstone = rows->add_deleted();
      tombstone->set_id(row["id"].asInt64());
      tombstone->set_deleted_at(row["deletedAt"].asInt64());
    }
  }
  if (body.find_last_created()) {
    const auto row = co_await repo.findLast(scope);
    if (row)
      toProto(*row, rows->mutable_last_created());
  }
  if (body.find_last_deleted()) {
    const auto row = co_await repo.findLastDeleted(scope);
    if (row) {
      rows->mutable_last_deleted()->set_id((*row)["id"].asInt64());
      rows->mutable_last_deleted()->set_deleted_at(
          (*row)["deletedAt"].asInt64());
    }
  }
  co_return;
}

}

IdentitySyncRpcService::IdentitySyncRpcService(Dependencies dependencies)
    : gate_(std::move(dependencies.gate))
{
}

grpc::ServerUnaryReactor* IdentitySyncRpcService::PullTable(
    grpc::CallbackServerContext* context,
    const argus::identity::v1::PullTableRequest* request,
    argus::identity::v1::PullTableResponse* response)
{
  if (const auto admission = gate_->admit(context, identity_callers::kPullTable);
      !admission.admitted()) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(argus::client::FleetCallerGate::refusal(admission.verdict));
    return reactor;
  }
  const auto caller = argus::client::callerUserId(context);
  const std::string roleName =
      argus::client::metadata(context, "x-argus-role");
  if (!caller || roleName.empty()) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::UNAUTHENTICATED,
                                 "identity metadata missing"));
    return reactor;
  }
  const auto table = tableOf(*request);
  if (!table) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::INVALID_ARGUMENT,
                                 "table branch is required"));
    return reactor;
  }

  const UserRole role = userRoleFromString(roleName);
  const bool roleActive = argus::client::callerRoleActive(context);
  if (!role_access::hasAccess({.role = role,
                               .table = *table,
                               .perm = RolePermission::Read,
                               .roleActive = roleActive})) {
    auto* reactor = context->DefaultReactor();
    reactor->Finish(grpc::Status(grpc::StatusCode::PERMISSION_DENIED,
                                 "table is not readable by this role"));
    return reactor;
  }

  SyncFilter scope;
  if (*table == TableName::User && !role_access::readsUserDirectory(role, roleActive))
    scope.userId = caller;

  const argus::identity::v1::PullTableRequest pull = *request;
  auto* reactor = context->DefaultReactor();
  auto* responseWriter = response;
  drogon::app().getLoop()->queueInLoop([this, reactor, pull, scope,
                                        responseWriter]() {
    drogon::async_run([this, reactor, pull, scope,
                       responseWriter]() -> drogon::Task<void> {
      try {
        switch (pull.table_case()) {
          case argus::identity::v1::PullTableRequest::kUser:
            co_await fill(FillInput{.body = pull.user(),
                                    .rows = responseWriter->mutable_user(),
                                    .repo = userRepository_,
                                    .scope = scope});
            break;
          case argus::identity::v1::PullTableRequest::kUserInvitation:
            co_await fill(
                FillInput{.body = pull.user_invitation(),
                          .rows = responseWriter->mutable_user_invitation(),
                          .repo = userInvitationRepository_,
                          .scope = scope});
            break;
          case argus::identity::v1::PullTableRequest::kPerson:
            co_await fill(FillInput{.body = pull.person(),
                                    .rows = responseWriter->mutable_person(),
                                    .repo = personRepository_,
                                    .scope = scope});
            break;
          default:
            co_return;
        }
        reactor->Finish(grpc::Status::OK);
      }
      catch (const std::exception& e) {
        LOG_WARN << "Identity sync RPC: PullTable failed: " << e.what();
        reactor->Finish(grpc::Status(grpc::StatusCode::INTERNAL,
                                     "identity could not complete the call"));
      }
      co_return;
    });
  });
  return reactor;
}
