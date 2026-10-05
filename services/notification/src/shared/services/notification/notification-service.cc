#include "notification-service.hxx"

#include <chrono>
#include <ctime>
#include <exception>
#include <drogon/utils/coroutine.h>
#include <errors/response-exception.hxx>
#include <notification/notification-delivery-status.hxx>
#include <notification/notification-delivery-sink.hxx>
#include <notification/notification-errors.hxx>
#include <nats/push-intent-sink.hxx>
#include <runtime/blocking-task.hxx>
#include <shared/services/notification/push-copy.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <sync/table-name.hxx>
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
constexpr int64_t kCommandRetentionS = 30LL * 24 * 3600;
constexpr int kDeliveryPage = 200;
constexpr int64_t kClaimLeaseS = 30;
}

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

NotificationService::PublishedPage NotificationService::publishPage(
    const PublishPageInput& input)
{
  PublishedPage page;
  page.sent.reserve(input.pending.size());
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
    if (!input.sink->publish(event)) {
      page.refused.push_back(delivery.deliveryId);
      continue;
    }
    page.sent.push_back(delivery.deliveryId);
    if (!input.pushSink || delivery.userId <= 0)
      continue;
    const PushCopy copy =
        push_copy::render({.lang = push_copy::langOf(delivery.data),
                           .urgency = push_copy::urgencyOf(delivery.data),
                           .call = false});
    input.pushSink->publish(
        PushIntent{.userId = delivery.userId,
                   .notificationId = delivery.notificationId,
                   .type = delivery.type,
                   .title = copy.title,
                   .body = copy.body,
                   .createdAtMs = delivery.createdAt * 1000,
                   .data = push_copy::minimalData(
                       {.notificationId = delivery.notificationId,
                        .data = delivery.data})});
  }
  return page;
}

drogon::Task<bool> NotificationService::deliverDurable(
    DeliverDurableInput input) const
{
  if (input.pushRequired && !input.pushSink)
    LOG_WARN << "push intents required but no push sink installed; "
                "settling deliveries without push";
  std::vector<int64_t> claimed;
  claimed.reserve(input.pending.size());
  for (const auto& delivery : input.pending)
    claimed.push_back(delivery.deliveryId);
  PublishPageInput page{.pending = std::move(input.pending),
                        .sink = std::move(input.sink),
                        .pushSink = std::move(input.pushSink)};
  PublishedPage published;
  std::exception_ptr failure;
  try {
    published = co_await BlockingTask<PublishedPage>(
        [page = std::move(page)]() { return publishPage(page); });
  }
  catch (...) {
    failure = std::current_exception();
  }
  if (failure) {
    co_await repository_.releaseClaims(claimed);
    std::rethrow_exception(failure);
  }
  co_await repository_.releaseClaims(published.refused);
  const int64_t settled = co_await repository_.markDelivered(
      {.deliveryIds = published.sent,
       .at = static_cast<int64_t>(std::time(nullptr))});
  co_return settled > 0;
}

drogon::Task<DeliverPendingOutcome> NotificationService::deliverPending()
    const
{
  if (!dependencies_.deliverySink) {
    LOG_INFO << "notification delivery sink not installed; "
                "intents stay pending";
    co_return DeliverPendingOutcome::NoSinkInstalled;
  }
  const auto sink = dependencies_.deliverySink;
  if (!co_await BlockingTask<bool>([sink]() { return sink->ensureStream(); }))
    co_return DeliverPendingOutcome::StreamUnavailable;
  while (true) {
    auto pending = co_await repository_.claimPending(
        {.limit = kDeliveryPage,
         .now = static_cast<int64_t>(std::time(nullptr)),
         .leaseS = kClaimLeaseS});
    if (pending.empty())
      co_return DeliverPendingOutcome::Settled;
    const bool progressed =
        co_await deliverDurable({.pending = std::move(pending),
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
  auto transaction = co_await db_transaction::begin(DbService::client());
  try {
    const auto changes = co_await repository_.markAsRead(
        {.userId = userId, .ids = ids, .client = transaction.get()});
    const AuditSink* sink = user_change::getNotificationSink();
    if (sink == nullptr) {
      if (!changes.empty())
        LOG_WARN << "user change sink not installed; drop notification audit";
    }
    else {
      for (const auto& change : changes) {
        co_await sink->publishAudit(UserAuditInput{
            .recordId = change.after.id,
            .tableName = TableName::Notification,
            .before = change.before.toJson(),
            .after = change.after.toJson(),
            .userIds = {userId},
            .client = transaction.get(),
        });
      }
    }
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(NotificationErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
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
    co_await repository_.purgeCommands(at - kCommandRetentionS);
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
