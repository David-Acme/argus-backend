#include "notification-service.hxx"

#include <chrono>
#include <ctime>
#include <drogon/utils/coroutine.h>
#include <shared/contracts/notification-delivery-sink.hxx>
#include <shared/contracts/push-intent-sink.hxx>
#include <shared/enums.hxx>
#include <trantor/utils/Logger.h>

namespace
{
int64_t nowMillis()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

constexpr int64_t kProbeRetentionS = 7LL * 24 * 3600;
} // namespace

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
  const DeliverPendingOutcome pendingOutcome = co_await deliverPending();
  if (pendingOutcome != DeliverPendingOutcome::Settled)
    LOG_WARN << "notification delivery deferred ("
             << deliverPendingOutcomeToString(pendingOutcome)
             << "): " << committed.expectedCount << " intents pending";
  co_return outcome;
}

drogon::Task<bool> NotificationService::deliverDurable(
    DeliverDurableInput input) const
{
  const bool pushArmed = input.pushSink != nullptr;
  if (input.pushRequired && !pushArmed)
    LOG_WARN << "push intents required but no push sink installed; "
                "settling deliveries without push";
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

drogon::Task<DeliverPendingOutcome> NotificationService::deliverPending()
    const
{
  if (!dependencies_.deliverySink) {
    LOG_INFO << "notification delivery sink not installed; "
                "intents stay pending";
    co_return DeliverPendingOutcome::NoSinkInstalled;
  }
  const NotificationDeliverySink& sink = *dependencies_.deliverySink;
  if (!sink.ensureStream())
    co_return DeliverPendingOutcome::StreamUnavailable;
  while (true) {
    const auto pending = co_await repository_.pendingDeliveries(200);
    if (pending.empty())
      co_return DeliverPendingOutcome::Settled;
    const bool progressed =
        co_await deliverDurable({.pending = pending,
                                 .sink = sink,
                                 .pushSink = dependencies_.pushSink,
                                 .pushRequired = dependencies_.pushRequired});
    if (!progressed)
      co_return DeliverPendingOutcome::PublishRefused;
  }
}

drogon::Task<int64_t> NotificationService::pendingBacklog() const
{
  co_return co_await repository_.pendingDeliveryCount();
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

drogon::Task<int64_t> NotificationService::ackDeliveries(
    int64_t userId, const std::vector<int64_t>& notificationIds) const
{
  if (notificationIds.empty())
    co_return 0;
  co_return co_await repository_.ackDeliveries(
      {.userId = userId,
       .notificationIds = notificationIds,
       .at = static_cast<int64_t>(std::time(nullptr)),
       .atMs = nowMillis()});
}

drogon::Task<DeliverySummary> NotificationService::deliverySummary(
    int64_t since, int64_t ackWindowS) const
{
  co_return co_await repository_.deliverySummary(
      {.since = since,
       .ackWindowS = ackWindowS > 0 ? ackWindowS : 86400,
       .now = static_cast<int64_t>(std::time(nullptr))});
}

drogon::Task<SelfTestState> NotificationService::runSelfTest() const
{
  SelfTestState state;
  const int64_t startMs = nowMillis();
  try {
    const int64_t at = static_cast<int64_t>(std::time(nullptr));
    const ProbeInsertResult inserted =
        co_await repository_.insertProbe(at, startMs);
    if (inserted.deliveryId > 0)
      co_await deliverPending();
    const std::string status = inserted.deliveryId > 0
                                   ? co_await repository_.deliveryState(
                                         inserted.deliveryId)
                                   : std::string{};
    state.at = at;
    state.ok = status == notificationDeliveryStatusToString(
                             NotificationDeliveryStatus::Sent);
    state.ms = nowMillis() - startMs;
    co_await repository_.recordProbe(
        {.at = state.at, .ok = state.ok, .ms = state.ms});
    co_await repository_.purgeProbes(at - kProbeRetentionS);
    if (!state.ok)
      LOG_WARN << "notification self-test probe did not settle (status '"
               << status << "')";
  }
  catch (const std::exception& error) {
    LOG_WARN << "notification self-test failed: " << error.what();
  }
  catch (...) {
    LOG_WARN << "notification self-test failed with unknown error";
  }
  co_return state;
}
