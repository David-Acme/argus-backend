#include "object-event-outbox-repository.hxx"

#include <drogon/orm/DbClient.h>
#include <shared/enums.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <trantor/utils/Logger.h>

#include <condition_variable>
#include <exception>
#include <mutex>
#include <utility>

using namespace object_event_outbox_query;

namespace
{

constexpr auto kCommitTimeout = std::chrono::seconds(5);

struct CommitState
{
  std::mutex mutex;
  std::condition_variable cv;
  bool done{false};
  bool committed{false};
};

// Drogon commits in the transaction destructor on the client loop; wait for
// the callback so the caller only continues once the write is durable.
bool commitAndWait(std::shared_ptr<drogon::orm::Transaction> transaction)
{
  if (!transaction)
    return false;
  auto state = std::make_shared<CommitState>();
  transaction->setCommitCallback([state](bool committed) {
    {
      std::lock_guard lock(state->mutex);
      state->done = true;
      state->committed = committed;
    }
    state->cv.notify_one();
  });
  transaction.reset();

  std::unique_lock lock(state->mutex);
  state->cv.wait_for(lock, kCommitTimeout, [&state]() { return state->done; });
  return state->done && state->committed;
}

} // namespace

ObjectEventEnqueueOutcome
ObjectEventOutboxRepository::enqueue(const ObjectEventEnqueueInput& input) const
{
  ObjectEventEnqueueOutcome outcome;
  if (input.eventId.empty() || input.payload.empty())
    return outcome;

  auto client = DbService::client();
  if (!client)
    return outcome;

  std::shared_ptr<drogon::orm::Transaction> transaction;
  try {
    transaction = client->newTransaction(drogon::orm::TransactionType::Immediate);
  }
  catch (const std::exception& e) {
    LOG_WARN << "Camera outbox: transaction unavailable (" << e.what() << ")";
    return outcome;
  }

  try {
    if (input.cooldownMs > 0) {
      for (const auto& name : input.cooldownClasses) {
        const auto rows =
            transaction->execSqlSync(SELECT_COOLDOWN.data(), input.cameraId,
                                     name);
        if (rows.empty())
          continue;
        const int64_t last = rows.front()["last_emit_ms"].as<int64_t>();
        if (last > 0 && input.nowMs - last < input.cooldownMs) {
          transaction->rollback();
          outcome.result = ObjectEventEnqueueResult::Suppressed;
          return outcome;
        }
      }
    }

    const auto inserted = transaction->execSqlSync(
        INSERT_EVENT.data(), input.eventId, input.payload,
        objectEventStatusToString(ObjectEventStatus::Pending), input.nowMs);
    outcome.inserted = inserted.affectedRows() > 0;
    if (outcome.inserted) {
      for (const auto& name : input.cooldownClasses)
        transaction->execSqlSync(UPSERT_COOLDOWN.data(), input.cameraId, name,
                                 input.nowMs);
    }

    if (input.maxPending > 0) {
      const auto count = transaction->execSqlSync(
          COUNT_PENDING.data(),
          objectEventStatusToString(ObjectEventStatus::Pending));
      const int64_t pending =
          count.empty() ? 0 : count.front()["total"].as<int64_t>();
      if (pending > input.maxPending) {
        const auto oldest = transaction->execSqlSync(
            OLDEST_PENDING.data(),
            objectEventStatusToString(ObjectEventStatus::Pending));
        if (!oldest.empty()) {
          const std::string dropped =
              oldest.front()["event_id"].as<std::string>();
          transaction->execSqlSync(
              MARK_OVERFLOW.data(),
              objectEventStatusToString(ObjectEventStatus::OverflowDropped),
              input.nowMs, dropped);
          ++outcome.overflowDropped;
          LOG_ERROR << "Camera outbox: pending cap reached; dropped oldest "
                       "observation "
                    << dropped;
        }
      }
    }
  }
  catch (const std::exception& e) {
    transaction->rollback();
    LOG_WARN << "Camera outbox: enqueue failed (" << e.what() << ")";
    return outcome;
  }

  if (!commitAndWait(std::move(transaction))) {
    LOG_WARN << "Camera outbox: commit did not complete for " << input.eventId;
    return outcome;
  }
  outcome.netPendingDelta =
      (outcome.inserted ? 1 : 0) - outcome.overflowDropped;
  outcome.result = ObjectEventEnqueueResult::Recorded;
  return outcome;
}

std::optional<ObjectEventRow> ObjectEventOutboxRepository::nextPending() const
{
  auto client = DbService::client();
  if (!client)
    return std::nullopt;
  const auto rows = client->execSqlSync(
      NEXT_PENDING.data(), objectEventStatusToString(ObjectEventStatus::Pending));
  if (rows.empty())
    return std::nullopt;
  return ObjectEventRow{.eventId = rows.front()["event_id"].as<std::string>(),
                        .payload = rows.front()["payload"].as<std::string>(),
                        .attempts = rows.front()["attempts"].as<int>()};
}

bool ObjectEventOutboxRepository::markSent(const std::string& eventId,
                                           int64_t at) const
{
  auto client = DbService::client();
  if (!client)
    return false;
  return client->execSqlSync(
             MARK_SENT.data(),
             objectEventStatusToString(ObjectEventStatus::Sent), at, eventId,
             objectEventStatusToString(ObjectEventStatus::Pending))
             .affectedRows() > 0;
}

bool ObjectEventOutboxRepository::recordAttempt(
    const std::string& eventId) const
{
  auto client = DbService::client();
  if (!client)
    return false;
  return client->execSqlSync(
             RECORD_ATTEMPT.data(), eventId,
             objectEventStatusToString(ObjectEventStatus::Pending))
             .affectedRows() > 0;
}

int64_t ObjectEventOutboxRepository::purgeExpiredCooldowns(
    int64_t olderThanMs) const
{
  auto client = DbService::client();
  if (!client)
    return 0;
  const auto result =
      client->execSqlSync(PURGE_COOLDOWNS.data(), olderThanMs);
  return result.affectedRows();
}

ObjectEventOutboxStats ObjectEventOutboxRepository::stats() const
{
  ObjectEventOutboxStats result;
  auto client = DbService::client();
  if (!client)
    return result;
  const auto rows = client->execSqlSync(
      OUTBOX_STATS.data(),
      objectEventStatusToString(ObjectEventStatus::Pending),
      objectEventStatusToString(ObjectEventStatus::Sent),
      objectEventStatusToString(ObjectEventStatus::OverflowDropped),
      objectEventStatusToString(ObjectEventStatus::Pending));
  if (rows.empty())
    return result;
  const auto& row = rows.front();
  if (!row["pending"].isNull())
    result.pending = row["pending"].as<int64_t>();
  if (!row["sent"].isNull())
    result.sent = row["sent"].as<int64_t>();
  if (!row["dropped"].isNull())
    result.overflowDropped = row["dropped"].as<int64_t>();
  if (!row["oldest"].isNull())
    result.oldestPendingAtMs = row["oldest"].as<int64_t>();
  return result;
}
