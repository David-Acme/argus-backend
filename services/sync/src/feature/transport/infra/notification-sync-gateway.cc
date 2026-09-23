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
