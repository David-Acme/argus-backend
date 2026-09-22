#include "change-outbox-repository.hxx"

#include "change-outbox-status.hxx"

#include <sqlite/db-service.hxx>
#include <trantor/utils/Logger.h>

using namespace change_outbox_query;

drogon::Task<ChangeOutboxDisposition>
ChangeOutboxRepository::enqueue(const ChangeOutboxEnqueueInput& input) const
{
  if (input.eventId.empty() || input.payload.empty())
    co_return ChangeOutboxDisposition::Failed;

  auto client = DbService::cameraClient();
  try {
    const auto inserted = co_await client->execSqlCoro(
        INSERT_EVENT, input.eventId, input.fingerprint, input.payload,
        changeOutboxStatusToString(ChangeOutboxStatus::Pending), input.at);
    if (inserted.affectedRows() > 0)
      co_return ChangeOutboxDisposition::Enqueued;

    const auto rows =
        co_await client->execSqlCoro(FIND_FINGERPRINT, input.eventId);
    if (rows.empty())
      co_return ChangeOutboxDisposition::Failed;
    if (rows.front()["fingerprint"].as<std::string>() == input.fingerprint)
      co_return ChangeOutboxDisposition::Replay;

    LOG_ERROR << "Camera change outbox: " << input.eventId
              << " already holds a different transition; not dispatching it";
    co_return ChangeOutboxDisposition::Conflict;
  }
  catch (const std::exception& e) {
    LOG_ERROR << "Camera change outbox: " << input.eventId
              << " could not be recorded (" << e.what() << ")";
    co_return ChangeOutboxDisposition::Failed;
  }
}

std::vector<ChangeOutboxRow>
ChangeOutboxRepository::pendingBatch(int limit) const
{
  if (limit <= 0)
    return {};
  auto client = DbService::cameraClient();
  const auto rows = client->execSqlSync(
      PENDING_BATCH,
      changeOutboxStatusToString(ChangeOutboxStatus::Pending), limit);
  std::vector<ChangeOutboxRow> pending;
  pending.reserve(rows.size());
  for (const auto& row : rows)
    pending.push_back({.eventId = row["event_id"].as<std::string>(),
                       .payload = row["payload"].as<std::string>(),
                       .attempts = row["attempts"].as<int>()});
  return pending;
}

bool ChangeOutboxRepository::markSent(const std::string& eventId,
                                      int64_t at) const
{
  auto client = DbService::cameraClient();
  return client->execSqlSync(
             MARK_SENT,
             changeOutboxStatusToString(ChangeOutboxStatus::Sent), at, eventId,
             changeOutboxStatusToString(ChangeOutboxStatus::Pending))
             .affectedRows() > 0;
}

bool ChangeOutboxRepository::recordAttempt(const std::string& eventId) const
{
  auto client = DbService::cameraClient();
  return client->execSqlSync(
             RECORD_ATTEMPT, eventId,
             changeOutboxStatusToString(ChangeOutboxStatus::Pending))
             .affectedRows() > 0;
}
