#include "notification-service.hxx"

#include <chrono>
#include <ctime>
#include <exception>
#include <drogon/utils/coroutine.h>
#include <errors/response-exception.hxx>
#include <notification/notification-delivery-status.hxx>
#include <notification/notification-delivery-sink.hxx>
#include <notification/notification-errors.hxx>
#include <runtime/blocking-task.hxx>
#include <shared/services/notification/delivery-page.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <sync/socket-emit-dto.hxx>
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

std::string kindOf(const Json::Value& data)
{
  if (!data.isObject() || !data["kind"].isString())
    return {};
  return data["kind"].asString();
}

SocketEmitDto tombstoneOf(const NotificationSchema& row)
{
  SocketEmitDto tombstone;
  tombstone.operation = SyncOperation::Delete;
  tombstone.option = TableName::Notification;
  tombstone.obj["id"] = static_cast<Json::Int64>(row.id);
  tombstone.obj["deletedAt"] =
      static_cast<Json::Int64>(row.deletedAt.value_or(0));
  return tombstone;
}

struct RemovalPublishInput
{
  std::vector<NotificationChange> changes;
  int64_t userId{0};
  drogon::orm::DbClient* client{nullptr};
};

drogon::Task<void> publishRemovals(const RemovalPublishInput& input)
{
  const UserEmitSink* sink = user_change::getNotificationSink();
  if (sink == nullptr) {
    LOG_WARN << "user change sink not installed; drop notification delete";
    co_return;
  }
  for (const auto& change : input.changes) {
    co_await sink->publishAudit({.recordId = change.after.id,
                                 .tableName = TableName::Notification,
                                 .before = change.before.toJson(),
                                 .after = change.after.toJson(),
                                 .userIds = {input.userId},
                                 .client = input.client});
    co_await sink->emitUsers({.userIds = {input.userId},
                              .body = tombstoneOf(change.after),
                              .client = input.client});
  }
}

std::string trimmed(std::string_view text)
{
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos)
    return {};
  const auto last = text.find_last_not_of(" \t\r\n");
  return std::string(text.substr(first, last - first + 1));
}

constexpr int64_t kProbeRetentionS = 7LL * 24 * 3600;
constexpr int64_t kCommandRetentionS = 30LL * 24 * 3600;
constexpr int64_t kDeletedRetentionS = 7LL * 24 * 3600;
}

NotificationService::NotificationService(Dependencies dependencies)
    : dependencies_(std::move(dependencies))
{
}

drogon::Task<NotificationCreateOutcome> NotificationService::createManyAndEmit(
    const NotificationBatchInput& input) const
{
  NotificationCreateOutcome outcome;
  if (dependencies_.kindAllowed && !dependencies_.kindAllowed(kindOf(input.notification.data)))
    co_return outcome;
  std::vector<NotificationCreateInput> inputs;
  inputs.reserve(input.userIds.size());
  for (const auto userId : input.userIds) {
    NotificationCreateInput entry = input.notification;
    entry.userId = userId;
    inputs.push_back(std::move(entry));
  }

  std::vector<int64_t> ringFollowingUserIds;
  const auto& episodeData = input.notification.data;
  if (dependencies_.alerts && dependencies_.ringPlanner && episodeData.isObject() &&
      episodeData.isMember("threadKey") && episodeData["threadKey"].isString() &&
      !episodeData["threadKey"].asString().empty()) {
    ringFollowingUserIds = co_await dependencies_.ringPlanner->ringFollowingUserIds(
        {.data = episodeData, .userIds = input.userIds});
  }

  const NotificationCommitResult committed =
      co_await repository_.createManyWithCommand(
          {.inputs = std::move(inputs),
           .commandId = input.commandId,
           .at = static_cast<int64_t>(std::time(nullptr)),
           .alerts = dependencies_.alerts,
           .ringFollowingUserIds = std::move(ringFollowingUserIds)});
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
  std::vector<int64_t> claimed;
  claimed.reserve(input.pending.size());
  for (const auto& delivery : input.pending)
    claimed.push_back(delivery.deliveryId);
  delivery_page::PageInput page{
      .pending = std::move(input.pending),
      .sink = std::move(input.sink),
      .deadline = std::chrono::steady_clock::now() + delivery_page::kPublishBudget,
      .clock = [] { return std::chrono::steady_clock::now(); }};
  delivery_page::PublishedPage published;
  std::exception_ptr failure;
  try {
    published = co_await BlockingTask<delivery_page::PublishedPage>(
        [page = std::move(page)]() { return delivery_page::publish(page); });
  }
  catch (...) {
    failure = std::current_exception();
  }
  if (failure) {
    co_await repository_.releaseClaims(claimed);
    std::rethrow_exception(failure);
  }
  co_await repository_.releaseClaims(published.unsent);
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
        {.limit = delivery_page::kPageSize,
         .now = static_cast<int64_t>(std::time(nullptr)),
         .leaseS = delivery_page::kClaimLeaseS});
    if (pending.empty())
      co_return DeliverPendingOutcome::Settled;
    const bool progressed =
        co_await deliverDurable({.pending = std::move(pending),
                                 .sink = sink});
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

drogon::Task<bool> NotificationService::remove(const NotificationRef& ref) const
{
  auto transaction = co_await db_transaction::begin(DbService::client());
  try {
    const auto change = co_await repository_.remove(
        {.id = ref.id, .userId = ref.userId, .client = transaction.get()});
    if (!change) {
      db_transaction::rollback(transaction);
      co_return false;
    }
    const RemovalPublishInput publish{.changes = {*change},
                                      .userId = ref.userId,
                                      .client = transaction.get()};
    co_await publishRemovals(publish);
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(NotificationErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  co_return true;
}

drogon::Task<bool>
NotificationService::removeThread(const NotificationThreadRef& ref) const
{
  std::string threadKey = trimmed(ref.threadKey);
  if (threadKey.empty())
    co_return false;
  auto transaction = co_await db_transaction::begin(DbService::client());
  try {
    const NotificationThreadInput thread{.userId = ref.userId,
                                         .threadKey = std::move(threadKey),
                                         .client = transaction.get()};
    auto changes = co_await repository_.removeThread(thread);
    if (changes.empty()) {
      db_transaction::rollback(transaction);
      co_return false;
    }
    const RemovalPublishInput publish{.changes = std::move(changes),
                                      .userId = ref.userId,
                                      .client = transaction.get()};
    co_await publishRemovals(publish);
    if (!co_await db_transaction::Commit(std::move(transaction)))
      throw ResponseException(NotificationErrors::ChangeNotRecorded);
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  co_return true;
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

drogon::Task<SelfTestState> NotificationService::runProbe() const
{
  SelfTestState state;
  const int64_t startMs = nowMillis();
  {
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
    co_await repository_.purgeDeleted(at - kDeletedRetentionS);
    if (!state.ok)
      LOG_WARN << "notification self-test probe did not settle (status '"
               << status << "')";
  }
  co_return state;
}

drogon::Task<SelfTestState> NotificationService::runSelfTest() const
{
  try {
    co_return co_await runProbe();
  }
  catch (const std::exception& error) {
    LOG_WARN << "notification self-test failed: " << error.what();
  }
  catch (...) {
    LOG_WARN << "notification self-test failed with unknown error";
  }
  co_return SelfTestState{};
}
