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

std::optional<ChangeOutboxRow> ChangeOutboxRepository::nextPending() const
{
  auto client = DbService::cameraClient();
  const auto rows = client->execSqlSync(
      NEXT_PENDING,
      changeOutboxStatusToString(ChangeOutboxStatus::Pending));
  if (rows.empty())
    return std::nullopt;
  return ChangeOutboxRow{.eventId = rows.front()["event_id"].as<std::string>(),
                         .payload = rows.front()["payload"].as<std::string>(),
                         .attempts = rows.front()["attempts"].as<int>()};
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
