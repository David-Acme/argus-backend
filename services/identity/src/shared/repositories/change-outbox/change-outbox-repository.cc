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

  const auto pooled = DbService::identityClient();
  auto* client = input.client ? input.client : pooled.get();
  const auto inserted = co_await client->execSqlCoro(
      INSERT_EVENT, input.eventId, input.subject, input.fingerprint,
      input.payload, changeOutboxStatusToString(ChangeOutboxStatus::Pending),
      input.at);
  if (inserted.affectedRows() > 0)
    co_return ChangeOutboxDisposition::Enqueued;

  const auto rows =
      co_await client->execSqlCoro(FIND_FINGERPRINT, input.eventId);
  if (rows.empty())
    throw std::runtime_error(
        "the change outbox ignored an insert it holds no row for");
  if (rows.front()["fingerprint"].as<std::string>() == input.fingerprint)
    co_return ChangeOutboxDisposition::Replay;

  LOG_ERROR << "Identity change outbox: " << input.eventId
            << " already holds a different transition; not dispatching it";
  co_return ChangeOutboxDisposition::Conflict;
}

drogon::Task<void>
ChangeOutboxRepository::enqueueAction(const ChangeOutboxActionInput& input) const
{
  if (input.eventId.empty() || input.payload.empty())
    throw std::invalid_argument(
        "a change outbox action row needs a msg id and a payload");

  const auto pooled = DbService::identityClient();
  auto* client = input.client ? input.client : pooled.get();
  co_await client->execSqlCoro(
      INSERT_ACTION, input.eventId, input.subject, input.fingerprint,
      input.payload, changeOutboxStatusToString(ChangeOutboxStatus::Pending),
      input.at);
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
    pending.push_back({.id = row["id"].as<int64_t>(),
                       .eventId = row["event_id"].as<std::string>(),
                       .subject = row["subject"].as<std::string>(),
                       .payload = row["payload"].as<std::string>(),
                       .attempts = row["attempts"].as<int>()});
  return pending;
}

bool ChangeOutboxRepository::markSent(int64_t id, int64_t at) const
{
  auto client = DbService::client();
  return client->execSqlSync(
             MARK_SENT, changeOutboxStatusToString(ChangeOutboxStatus::Sent),
             at, id, changeOutboxStatusToString(ChangeOutboxStatus::Pending))
             .affectedRows() > 0;
}

bool ChangeOutboxRepository::recordAttempt(int64_t id) const
{
  auto client = DbService::client();
  return client->execSqlSync(
             RECORD_ATTEMPT, id,
             changeOutboxStatusToString(ChangeOutboxStatus::Pending))
             .affectedRows() > 0;
}

int64_t ChangeOutboxRepository::purgeSent(int64_t olderThanMs) const
{
  auto client = DbService::client();
  const auto result =
      client->execSqlSync(PURGE_SENT,
                          changeOutboxStatusToString(ChangeOutboxStatus::Sent),
                          olderThanMs);
  return static_cast<int64_t>(result.affectedRows());
}
