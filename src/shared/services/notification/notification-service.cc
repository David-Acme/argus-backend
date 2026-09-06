#include "notification-service.hxx"

#include <drogon/utils/coroutine.h>
#include <shared/contracts/sync-operation.hxx>
#include <shared/enums.hxx>
#include <shared/schemas/notification/notification-schema.hxx>
#include <trantor/utils/Logger.h>

drogon::Task<void>
NotificationService::createAndEmit(int64_t userId,
                                   const NotificationCreateInput& input) const
{
  co_await createAndEmitMany({userId}, input);
}

drogon::Task<void> NotificationService::createAndEmitMany(
    const std::vector<int64_t>& userIds,
    const NotificationCreateInput& input) const
{
  if (userIds.empty())
    co_return;

  auto repo = repository_;
  drogon::async_run([repo, userIds, input]() -> drogon::Task<void> {
    try {
      std::vector<NotificationCreateInput> inputs;
      inputs.reserve(userIds.size());
      for (const auto userId : userIds) {
        NotificationCreateInput entry = input;
        entry.userId = userId;
        inputs.push_back(std::move(entry));
      }

      const auto notifications = co_await repo.createMany(inputs);
      for (const auto& notification : notifications) {
        SocketEmitDto emit;
        emit.operation = SyncOperation::Add;
        emit.option = TableName::Notification;
        emit.obj = notification.toJson();
        if (const auto* sink = user_change::getNotificationSink())
          sink->emitUser(notification.userId, emit);
        else
          LOG_WARN << "user change sink not installed; drop notification emit";
      }
    }
    catch (const std::exception& e) {
      LOG_ERROR << "NotificationService::createAndEmitMany error: " << e.what();
    }
  });
  co_return;
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
