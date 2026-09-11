#include "notification-service.hxx"

#include <drogon/utils/coroutine.h>
#include <shared/contracts/push-intent-sink.hxx>
#include <shared/contracts/sync-operation.hxx>
#include <shared/enums.hxx>
#include <shared/schemas/notification/notification-schema.hxx>
#include <trantor/utils/Logger.h>

drogon::Task<std::vector<NotificationSchema>>
NotificationService::createManyAndEmit(
    const std::vector<int64_t>& userIds,
    const NotificationCreateInput& input) const
{
  std::vector<NotificationSchema> notifications;
  if (userIds.empty())
    co_return notifications;

  std::vector<NotificationCreateInput> inputs;
  inputs.reserve(userIds.size());
  for (const auto userId : userIds) {
    NotificationCreateInput entry = input;
    entry.userId = userId;
    inputs.push_back(std::move(entry));
  }

  notifications = co_await repository_.createMany(inputs);
  for (const auto& notification : notifications) {
    SocketEmitDto emit;
    emit.operation = SyncOperation::Add;
    emit.option = TableName::Notification;
    emit.obj = notification.toJson();
    if (const auto* sink = user_change::getNotificationSink())
      sink->emitUser(notification.userId, emit);
    else
      LOG_WARN << "user change sink not installed; drop notification emit";
    if (const auto* intents = push_intent::getSink()) {
      intents->publish(PushIntent{
          .userId = notification.userId,
          .notificationId = notification.id,
          .type = notification.type,
          .title = notification.title,
          .body = notification.body,
          .createdAtMs = notification.createdAt * 1000,
      });
    }
  }
  co_return notifications;
}

drogon::Task<void>
NotificationService::markAsRead(int64_t userId,
                                const std::vector<int64_t>& ids) const
{
  const auto changes = co_await repository_.markAsRead(userId, ids);
  const auto* sink = user_change::getNotificationSink();
  for (const auto& change : changes) {
    if (!sink) {
      LOG_WARN << "user change sink not installed; drop notification audit";
      break;
    }
    co_await sink->publishAudit(UserAuditInput{
        .recordId = change.after.id,
        .tableName = TableName::Notification,
        .before = change.before.toJson(),
        .after = change.after.toJson(),
        .userIds = {userId},
    });
  }
  co_return;
}
