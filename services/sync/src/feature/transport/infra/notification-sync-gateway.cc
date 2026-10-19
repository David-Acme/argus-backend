#include "notification-sync-gateway.hxx"
#include <shared/infra/notification-row-json.hxx>

#include <errors/response-exception.hxx>
#include <sync/sync-errors.hxx>
#include <config/config-service.hxx>

#include <text/json-util.hxx>
#include <runtime/blocking-task.hxx>
#include <string>
#include <auth/user-role.hxx>
#include <utility>

namespace
{
using argus::notification::v1::NotificationRow;
using argus::notification::v1::PullNotificationsRequest;

argus::client::CallerIdentity identityFor(const JwtContext& ctx)
{
  return {.userId = ctx.sub,
          .role = userRoleToString(ctx.role),
          .device = ctx.deviceHash};
}

argus::notification::v1::SyncRange toRange(const SyncFilter& filter)
{
  argus::notification::v1::SyncRange range;
  if (filter.startTime)
    range.set_start_time(*filter.startTime);
  if (filter.startId)
    range.set_start_id(*filter.startId);
  if (filter.endTime)
    range.set_end_time(*filter.endTime);
  return range;
}

Json::Value rowToJson(const NotificationRow& row)
{
  const std::optional<int64_t> readAt =
      row.has_read_at() ? std::optional<int64_t>(row.read_at()) : std::nullopt;
  return NotificationRowJson{.id = row.id(),
                             .userId = row.user_id(),
                             .type = row.type(),
                             .title = row.title(),
                             .body = row.body(),
                             .data = json_util::fromString(row.data()),
                             .isRead = row.is_read(),
                             .readAt = readAt,
                             .createdAt = row.created_at()}
      .toJson();
}

Json::Value conversationRowToJson(const argus::notification::v1::ConversationMessageRow& row)
{
  Json::Value json(Json::objectValue);
  json["id"] = static_cast<Json::Int64>(row.id());
  json["userId"] = static_cast<Json::Int64>(row.user_id());
  json["counterpartId"] = static_cast<Json::Int64>(row.counterpart_id());
  json["counterpartName"] = row.counterpart_name();
  json["direction"] = row.direction();
  json["kind"] = row.kind();
  json["body"] = row.body();
  json["leg"] = row.has_leg() ? Json::Value(row.leg()) : Json::Value();
  json["state"] = row.state();
  json["messageKey"] = row.message_key();
  json["replyToKey"] = row.has_reply_to_key() ? Json::Value(row.reply_to_key()) : Json::Value();
  json["isRead"] = row.is_read();
  json["readAt"] = row.has_read_at() ? Json::Value(static_cast<Json::Int64>(row.read_at())) : Json::Value();
  json["createdAt"] = static_cast<Json::Int64>(row.created_at());
  json["updatedAt"] =
      row.has_updated_at() ? Json::Value(static_cast<Json::Int64>(row.updated_at())) : Json::Value();
  json["deletedAt"] = Json::Value();
  return json;
}

Json::Value tombstoneToJson(const argus::notification::v1::ConversationTombstone& tombstone)
{
  Json::Value json(Json::objectValue);
  json["id"] = static_cast<Json::Int64>(tombstone.id());
  json["deletedAt"] = static_cast<Json::Int64>(tombstone.deleted_at());
  return json;
}

ResponseException unavailable()
{
  return ResponseException(503, SyncErrors::NotificationSyncUnavailable);
}
}

NotificationSyncGateway::NotificationSyncGateway(std::string target)
    : client_(std::make_shared<NotificationClient>(NotificationClientConfig{
          .target = std::move(target),
          .credential = ConfigService::getString("notifications.credential")}))
{
}

drogon::Task<std::vector<Json::Value>>
NotificationSyncGateway::find(const JwtContext& ctx,
                              const SyncFilter& filter) const
{
  const argus::client::CallerIdentity identity = identityFor(ctx);

  PullNotificationsRequest request;
  auto* notification = request.mutable_notification();
  notification->set_required_create(true);
  *notification->mutable_created() = toRange(filter);

  const auto response =
      co_await BlockingTask<NotificationPullResult>([this, request, identity]() {
        return client_->pullNotifications(request, identity);
      });
  if (response.outcome != NotificationRpcOutcome::Success)
    throw unavailable();

  std::vector<Json::Value> rows;
  rows.reserve(response.response.created_size());
  for (const auto& row : response.response.created())
    rows.push_back(rowToJson(row));
  co_return rows;
}

drogon::Task<std::optional<Json::Value>>
NotificationSyncGateway::findLast(const JwtContext& ctx) const
{
  const argus::client::CallerIdentity identity = identityFor(ctx);

  PullNotificationsRequest request;
  request.mutable_notification()->set_find_last_created(true);

  const auto response =
      co_await BlockingTask<NotificationPullResult>([this, request, identity]() {
        return client_->pullNotifications(request, identity);
      });
  if (response.outcome != NotificationRpcOutcome::Success)
    throw unavailable();
  if (!response.response.has_last_created())
    co_return std::nullopt;
  co_return rowToJson(response.response.last_created());
}

drogon::Task<ConversationPage>
NotificationSyncGateway::conversation(const JwtContext& ctx, const ConversationPull& pull) const
{
  const argus::client::CallerIdentity identity = identityFor(ctx);

  argus::notification::v1::PullConversationRequest request;
  auto* wire = request.mutable_pull();
  wire->set_required_create(pull.requiredCreate);
  wire->set_find_last_created(pull.findLastCreated);
  *wire->mutable_created() = toRange(pull.created);
  wire->set_required_deleted(pull.requiredDeleted);
  wire->set_find_last_deleted(pull.findLastDeleted);
  *wire->mutable_deleted() = toRange(pull.deleted);

  const auto response = co_await BlockingTask<NotificationConversationPullResult>(
      [this, request, identity]() { return client_->pullConversation(request, identity); });
  if (response.outcome != NotificationRpcOutcome::Success)
    throw unavailable();

  ConversationPage page;
  page.created.reserve(static_cast<std::size_t>(response.response.created_size()));
  for (const auto& row : response.response.created())
    page.created.push_back(conversationRowToJson(row));
  if (response.response.has_last_created())
    page.lastCreated = conversationRowToJson(response.response.last_created());
  page.deleted.reserve(static_cast<std::size_t>(response.response.deleted_size()));
  for (const auto& tombstone : response.response.deleted())
    page.deleted.push_back(tombstoneToJson(tombstone));
  if (response.response.has_last_deleted())
    page.lastDeleted = tombstoneToJson(response.response.last_deleted());
  co_return page;
}
