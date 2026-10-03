#include <feature/monitor/camera-presence.hxx>

#include <sqlite/db-service.hxx>
#include <sqlite/transaction.hxx>
#include <sync/camera-change-sink.hxx>
#include <trantor/utils/Logger.h>

#include <exception>

std::optional<bool> CameraPresenceRecorder::transitionFor(const CameraPresenceSample& sample)
{
  std::scoped_lock lock(mutex_);
  Presence& presence = presence_[sample.cameraId];
  if (sample.reachable) {
    presence.misses = 0;
    if (presence.online == true)
      return std::nullopt;
    return true;
  }
  ++presence.misses;
  if (presence.online == false || presence.misses < kMissesBeforeOffline)
    return std::nullopt;
  return false;
}

void CameraPresenceRecorder::remember(PresenceChange change)
{
  std::scoped_lock lock(mutex_);
  presence_[change.cameraId].online = change.online;
}

drogon::Task<void> CameraPresenceRecorder::record(CameraPresenceSample sample)
{
  const auto online = transitionFor(sample);
  if (!online)
    co_return;
  const PresenceChange change{.cameraId = sample.cameraId, .online = *online};
  try {
    if (co_await persist(change))
      remember(change);
  }
  catch (const std::exception& error) {
    LOG_WARN << "Camera presence: camera " << change.cameraId
             << " could not be marked " << (change.online ? "online" : "offline")
             << " (" << error.what() << ")";
  }
}

drogon::Task<bool> CameraPresenceRecorder::persist(PresenceChange change) const
{
  auto transaction = co_await db_transaction::begin(DbService::cameraClient());
  try {
    const auto found = co_await repository_.findById(change.cameraId, transaction.get());
    if (!found || found->isOnline == change.online) {
      db_transaction::rollback(transaction);
      co_return true;
    }
    const CameraSchema before = *found;

    CameraUpdateInput input;
    input.isOnline = change.online;
    input.client = transaction.get();
    const CameraSchema after = co_await repository_.update(change.cameraId, input);
    if (after.id == 0) {
      db_transaction::rollback(transaction);
      co_return false;
    }

    if (const auto* sink = camera_change::getSink()) {
      co_await sink->publishAudit({.recordId = after.id,
                                   .tableName = TableName::Camera,
                                   .before = before.toJson(),
                                   .after = after.toJson(),
                                   .actorId = std::nullopt,
                                   .client = transaction.get()});
    }
    if (!co_await db_transaction::Commit(std::move(transaction)))
      co_return false;
  }
  catch (...) {
    db_transaction::rollback(transaction);
    throw;
  }
  LOG_INFO << "Camera presence: camera " << change.cameraId << " is "
           << (change.online ? "online" : "offline");
  co_return true;
}
