#include "change-outbox-repository.hxx"

#include "change-outbox-status.hxx"

#include <sqlite/db-service.hxx>
#include <stdexcept>

using namespace change_outbox_query;

drogon::Task<void>
ChangeOutboxRepository::enqueueAction(const ChangeOutboxActionInput& input) const
{
  if (input.subject.empty() || input.payload.empty())
    throw std::invalid_argument(
        "a change outbox action row needs a subject and a payload");

  const auto pooled = DbService::client();
  auto* client = input.client ? input.client : pooled.get();
  co_await client->execSqlCoro(
      INSERT_ACTION, input.subject, input.fingerprint, input.payload,
      changeOutboxStatusToString(ChangeOutboxStatus::Pending), input.at);
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
