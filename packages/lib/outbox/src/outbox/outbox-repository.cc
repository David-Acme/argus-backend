#include "outbox-repository.hxx"

#include "outbox-status.hxx"

#include <exception>
#include <stdexcept>
#include <trantor/utils/Logger.h>
#include <utility>

using namespace outbox_query;

namespace outbox
{

namespace
{
bool positive(const drogon::orm::Result& rows)
{
  return !rows.empty() && rows.front()["total"].as<int64_t>() > 0;
}
}

OutboxRepository::OutboxRepository(ClientAccessor client)
    : client_(std::move(client))
{
}

drogon::orm::DbClientPtr OutboxRepository::client() const
{
  auto client = client_();
  if (!client)
    throw std::runtime_error("the change outbox has no database client");
  return client;
}

bool OutboxRepository::hasColumn(const char* column) const
{
  return positive(
      client()->execSqlSync(COUNT_OUTBOX_COLUMN, std::string(column)));
}

bool OutboxRepository::migrateSchema() const
{
  try {
    if (!positive(client()->execSqlSync(COUNT_OUTBOX_TABLE)))
      return true;
    if (!hasColumn("event_id"))
      client()->execSqlSync(ADD_EVENT_ID_COLUMN);
    if (!hasColumn("subject"))
      client()->execSqlSync(ADD_SUBJECT_COLUMN);
    return true;
  }
  catch (const std::exception& error) {
    LOG_ERROR << "change_outbox migration failed: " << error.what();
    return false;
  }
}

drogon::Task<OutboxDisposition>
OutboxRepository::insert(const OutboxInsert& input) const
{
  if (input.eventId.empty() || input.subject.empty() || input.payload.empty())
    throw std::invalid_argument(
        "a change outbox row needs a msg id, a subject and a payload");

  const auto pooled = input.client ? nullptr : client();
  auto* db = input.client ? input.client : pooled.get();
  const auto inserted = co_await db->execSqlCoro(
      INSERT_EVENT, input.eventId, input.subject, input.fingerprint,
      input.payload, outboxStatusToString(OutboxStatus::Pending), input.at);
  if (inserted.affectedRows() > 0)
    co_return OutboxDisposition::Enqueued;

  const auto rows = co_await db->execSqlCoro(FIND_FINGERPRINT, input.eventId);
  if (rows.empty())
    throw std::runtime_error(
        "the change outbox ignored an insert it holds no row for");
  if (rows.front()["fingerprint"].as<std::string>() == input.fingerprint)
    co_return OutboxDisposition::Replay;
  co_return OutboxDisposition::Conflict;
}

std::vector<OutboxRow> OutboxRepository::pendingBatch(int limit) const
{
  if (limit <= 0)
    return {};
  const auto rows = client()->execSqlSync(
      PENDING_BATCH, outboxStatusToString(OutboxStatus::Pending), limit);
  std::vector<OutboxRow> pending;
  pending.reserve(rows.size());
  for (const auto& row : rows)
    pending.push_back({.id = row["row_id"].as<int64_t>(),
                       .eventId = row["event_id"].as<std::string>(),
                       .subject = row["subject"].as<std::string>(),
                       .payload = row["payload"].as<std::string>(),
                       .attempts = row["attempts"].as<int>()});
  return pending;
}

bool OutboxRepository::markSent(int64_t id, int64_t at) const
{
  return client()
             ->execSqlSync(MARK_SENT, outboxStatusToString(OutboxStatus::Sent),
                           at, id, outboxStatusToString(OutboxStatus::Pending))
             .affectedRows() > 0;
}

bool OutboxRepository::recordAttempt(int64_t id) const
{
  return client()
             ->execSqlSync(RECORD_ATTEMPT, id,
                           outboxStatusToString(OutboxStatus::Pending))
             .affectedRows() > 0;
}

int64_t OutboxRepository::purgeSent(int64_t olderThanMs) const
{
  const auto result = client()->execSqlSync(
      PURGE_SENT, outboxStatusToString(OutboxStatus::Sent), olderThanMs);
  return static_cast<int64_t>(result.affectedRows());
}

}
