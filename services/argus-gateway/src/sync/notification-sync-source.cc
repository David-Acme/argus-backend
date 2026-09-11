#include "notification-sync-source.hxx"

#include <config/app-config.hxx>
#include <shared/enums.hxx>
#include <shared/exceptions/response-exception.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <shared/wrapper/blocking-task/blocking-task.hxx>
#include <string>
#include <utility>

namespace
{
using argus::notification::v1::NotificationRow;
using argus::notification::v1::PullNotificationsRequest;
using argus::notification::v1::PullNotificationsResponse;

argus::sdk::CallerIdentity identityFor(const JwtContext& ctx)
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
  Json::Value json(Json::objectValue);
  json["id"] = Json::Int64(row.id());
  json["userId"] = Json::Int64(row.user_id());
  json["type"] = row.type();
  json["title"] = row.title();
  json["body"] = row.body();
  json["data"] = json_util::fromString(row.data());
  json["isRead"] = row.is_read();
  json["readAt"] = row.has_read_at() ? Json::Value(Json::Int64(row.read_at()))
                                     : Json::Value();
  json["createdAt"] = Json::Int64(row.created_at());
  return json;
}

ResponseException unavailable()
{
  return ResponseException({.message = "Notification sync unavailable",
                            .statusCode = 503,
                            .errorCode =
                                AppConfig::ERROR_CODE_SERVICE_UNAVAILABLE});
}
} // namespace

NotificationSyncGateway::NotificationSyncGateway(std::string target)
    : client_(std::make_shared<NotificationClient>(std::move(target)))
{
}

drogon::Task<std::vector<Json::Value>>
NotificationSyncGateway::find(const JwtContext& ctx,
                              const SyncFilter& filter) const
{
  const argus::sdk::CallerIdentity identity = identityFor(ctx);

  PullNotificationsRequest request;
  auto* notification = request.mutable_notification();
  notification->set_required_create(true);
  *notification->mutable_created() = toRange(filter);

  const auto response =
      co_await BlockingTask<std::optional<PullNotificationsResponse>>(
          [this, request, identity]() {
            return client_->pullNotifications(request, identity);
          });
  if (!response)
    throw unavailable();

  std::vector<Json::Value> rows;
  rows.reserve(response->created_size());
  for (const auto& row : response->created())
    rows.push_back(rowToJson(row));
  co_return rows;
}

drogon::Task<std::optional<Json::Value>>
NotificationSyncGateway::findLast(const JwtContext& ctx) const
{
  const argus::sdk::CallerIdentity identity = identityFor(ctx);

  PullNotificationsRequest request;
  request.mutable_notification()->set_find_last_created(true);

  const auto response =
      co_await BlockingTask<std::optional<PullNotificationsResponse>>(
          [this, request, identity]() {
            return client_->pullNotifications(request, identity);
          });
  if (!response)
    throw unavailable();
  if (!response->has_last_created())
    co_return std::nullopt;
  co_return rowToJson(response->last_created());
}
