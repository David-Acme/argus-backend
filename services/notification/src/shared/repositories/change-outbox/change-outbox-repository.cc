#include "change-outbox-repository.hxx"

#include "change-outbox-status.hxx"

#include <sqlite/db-service.hxx>
#include <stdexcept>
#include <trantor/utils/Logger.h>

using namespace change_outbox_query;

drogon::Task<ChangeOutboxDisposition>
ChangeOutboxRepository::enqueue(const ChangeOutboxEnqueueInput& input) const
{
  if (input.eventId.empty() || input.payload.empty())
    throw std::invalid_argument(
        "a change outbox row needs an event id and a payload");

  const auto pooled = DbService::client();
  auto* client = input.client ? input.client : pooled.get();
  const auto inserted = co_await client->execSqlCoro(
      INSERT_EVENT, input.eventId, input.fingerprint, input.payload,
      changeOutboxStatusToString(ChangeOutboxStatus::Pending), input.at);
  if (inserted.affectedRows() > 0)
    co_return ChangeOutboxDisposition::Enqueued;

  const auto rows =
      co_await client->execSqlCoro(FIND_FINGERPRINT, input.eventId);
  if (rows.empty())
    throw std::runtime_error(
        "the change outbox ignored an insert it holds no row for");
  if (rows.front()["fingerprint"].as<std::string>() == input.fingerprint)
    co_return ChangeOutboxDisposition::Replay;

  LOG_ERROR << "Notification change outbox: " << input.eventId
            << " already holds a different transition; not dispatching it";
  co_return ChangeOutboxDisposition::Conflict;
}

std::vector<ChangeOutboxRow>
ChangeOutboxRepository::pendingBatch(int limit) const
{
  if (limit <= 0)
    return {};
  auto client = DbService::client();
  const auto rows = client->execSqlSync(
      PENDING_BATCH, changeOutboxStatusToString(ChangeOutboxStatus::Pending),
      limit);
  std::vector<ChangeOutboxRow> pending;
  pending.reserve(rows.size());
  for (const auto& row : rows)
    pending.push_back(
        {.eventId = row["event_id"].as<std::string>(),
         .payload = row["payload"].as<std::string>(),
         .attempts = row["attempts"].as<int>()});
  return pending;
}

bool ChangeOutboxRepository::markSent(const std::string& eventId,
                                      int64_t at) const
{
  auto client = DbService::client();
  return client->execSqlSync(
             MARK_SENT,
             changeOutboxStatusToString(ChangeOutboxStatus::Sent), at, eventId,
             changeOutboxStatusToString(ChangeOutboxStatus::Pending))
             .affectedRows() > 0;
}

bool ChangeOutboxRepository::recordAttempt(const std::string& eventId) const
{
  auto client = DbService::client();
  return client->execSqlSync(
             RECORD_ATTEMPT, eventId,
             changeOutboxStatusToString(ChangeOutboxStatus::Pending))
             .affectedRows() > 0;
}
