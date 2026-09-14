#include "notification-service.hxx"

#include <drogon/utils/coroutine.h>
#include <shared/contracts/notification-delivery-sink.hxx>
#include <shared/contracts/push-intent-sink.hxx>
#include <shared/enums.hxx>
#include <stdexcept>
#include <trantor/utils/Logger.h>

NotificationService::NotificationService(Dependencies dependencies)
    : dependencies_(std::move(dependencies))
{
}

drogon::Task<NotificationCreateOutcome> NotificationService::createManyAndEmit(
    const NotificationBatchInput& input) const
{
  NotificationCreateOutcome outcome;
  std::vector<NotificationCreateInput> inputs;
  inputs.reserve(input.userIds.size());
  for (const auto userId : input.userIds) {
    NotificationCreateInput entry = input.notification;
    entry.userId = userId;
    inputs.push_back(std::move(entry));
  }

  const NotificationCommitResult committed =
      co_await repository_.createManyWithCommand(
          {.inputs = std::move(inputs),
           .commandId = input.commandId,
           .at = static_cast<int64_t>(std::time(nullptr))});
  outcome.duplicate = committed.duplicate;
  outcome.createdCount = committed.expectedCount;
  co_await deliverPending();
  co_return outcome;
}

drogon::Task<bool> NotificationService::deliverDurable(
    DeliverDurableInput input) const
{
  if (input.pushRequired && !input.pushSink)
    throw std::runtime_error("push intent sink not installed");
  bool progressed = false;
  for (const auto& delivery : input.pending) {
    const NotificationDeliveryEvent event{
        .deliveryId = delivery.deliveryId,
        .notificationId = delivery.notificationId,
        .userId = delivery.userId,
        .type = delivery.type,
        .title = delivery.title,
        .body = delivery.body,
        .data = delivery.data,
        .createdAt = delivery.createdAt};
    if (!input.sink.publish(event))
      continue;
    if (input.pushSink) {
      input.pushSink->publish(PushIntent{.userId = delivery.userId,
                                         .notificationId =
                                             delivery.notificationId,
                                         .type = delivery.type,
                                         .title = delivery.title,
                                         .body = delivery.body,
                                         .createdAtMs = delivery.createdAt *
                                                          1000});
    }
    if (co_await repository_.markDelivered(delivery.deliveryId,
                                           static_cast<int64_t>(
                                               std::time(nullptr))))
      progressed = true;
  }
  co_return progressed;
}

drogon::Task<void> NotificationService::deliverPending() const
{
  if (!dependencies_.deliverySink)
    throw std::runtime_error("notification delivery sink not installed");
  const NotificationDeliverySink& sink = *dependencies_.deliverySink;
  if (!sink.ensureStream())
    co_return;
  while (true) {
    const auto pending = co_await repository_.pendingDeliveries(200);
    if (pending.empty())
      co_return;
    const bool progressed =
        co_await deliverDurable({.pending = pending,
                                 .sink = sink,
                                 .pushSink = dependencies_.pushSink,
                                 .pushRequired = dependencies_.pushRequired});
    if (!progressed)
      co_return;
  }
}

drogon::Task<void>
NotificationService::markAsRead(int64_t userId,
                                const std::vector<int64_t>& ids) const
{
  const auto changes = co_await repository_.markAsRead(userId, ids);
  if (user_change::getNotificationSink() == nullptr) {
    if (!changes.empty())
      LOG_WARN << "user change sink not installed; drop notification audit";
    co_return;
  }
  const UserChangeSink& sink = *user_change::getNotificationSink();
  for (const auto& change : changes) {
    co_await sink.publishAudit(UserAuditInput{
        .recordId = change.after.id,
        .tableName = TableName::Notification,
        .before = change.before.toJson(),
        .after = change.after.toJson(),
        .userIds = {userId},
    });
  }
  co_return;
}
