#include "privacy-state.hxx"

#include <algorithm>
#include <chrono>
#include <drogon/HttpAppFramework.h>
#include <drogon/drogon.h>
#include <shared/services/camera-driver/camera-scene-log.hxx>
#include <shared/services/privacy-notice/privacy-notice.hxx>
#include <shared/services/stream/snapshot-store.hxx>
#include <shared/services/stream/stream-hub.hxx>
#include <sync/camera-change-sink.hxx>
#include <sync/resource-change.hxx>
#include <sync/table-name.hxx>
#include <text/json-util.hxx>

namespace
{
constexpr const char* kPrivacyEnabledKey = "privacyEnabled";

int64_t epochNowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

CameraPrivacyState observedState(const Json::Value& status)
{
  const Json::Value& enabled = status[kPrivacyEnabledKey];
  if (!enabled.isBool())
    return CameraPrivacyState::Unknown;
  return enabled.asBool() ? CameraPrivacyState::On : CameraPrivacyState::Off;
}
}

PrivacyStateService& PrivacyStateService::instance()
{
  static PrivacyStateService service;
  return service;
}

void PrivacyStateService::seed(const std::vector<CameraSchema>& cameras)
{
  std::scoped_lock lock(mutex_);
  for (const auto& camera : cameras)
  {
    if (camera.id <= 0)
      continue;
    const CameraPrivacyState state = camera.privacyState;
    const bool obeyedNow =
        camera.privacyObeyed || state == CameraPrivacyState::On;
    const CameraPrivacyState confirmed =
        state == CameraPrivacyState::Unknown
            ? (obeyedNow ? CameraPrivacyState::On : CameraPrivacyState::Unknown)
            : state;
    entries_[camera.id] = {
        .state = state,
        .confirmed = confirmed,
        .persisted = state,
        .noticed = obeyedNow && state == CameraPrivacyState::Unknown,
        .persistedObeyed = camera.privacyObeyed,
        .writing = false};
  }
}

bool PrivacyStateService::obeyed(const Entry& entry)
{
  return entry.confirmed == CameraPrivacyState::On &&
         entry.state != CameraPrivacyState::Off;
}

bool PrivacyStateService::doubted(const Entry& entry)
{
  return entry.state == CameraPrivacyState::Unknown &&
         entry.confirmed == CameraPrivacyState::On;
}

CameraPrivacyState PrivacyStateService::stateOf(int64_t cameraId) const
{
  std::scoped_lock lock(mutex_);
  const auto it = entries_.find(cameraId);
  return it == entries_.end() ? CameraPrivacyState::Unknown : it->second.state;
}

bool PrivacyStateService::masked(int64_t cameraId) const
{
  std::scoped_lock lock(mutex_);
  const auto it = entries_.find(cameraId);
  return it != entries_.end() && obeyed(it->second);
}

bool PrivacyStateService::maskedOf(const CameraSchema& camera) const
{
  return camera.id > 0 && masked(camera.id);
}

bool PrivacyStateService::privacyCapable(const CameraSchema& camera)
{
  const Json::Value list = json_util::fromString(camera.capabilities);
  if (!list.isArray())
    return false;
  return std::ranges::any_of(list, [](const Json::Value& name) {
    return name.isString() && name.asString() == "privacy";
  });
}

void PrivacyStateService::forget(int64_t cameraId)
{
  std::scoped_lock lock(mutex_);
  entries_.erase(cameraId);
}

drogon::Task<bool> PrivacyStateService::persistState(PrivacyPersistInput input)
{
  bool written = false;
  if (drogon::app().isRunning())
  {
    try
    {
      CameraUpdateInput update;
      update.privacyState = input.state;
      update.privacyObeyed = input.obeyed;
      const auto updated =
          co_await repository_.update(input.cameraId, update);
      written = updated.id == input.cameraId;
    }
    catch (const std::exception& error)
    {
      LOG_WARN << "Camera privacy: the state of camera " << input.cameraId
               << " could not be persisted: " << error.what();
    }
  }
  std::scoped_lock lock(mutex_);
  const auto it = entries_.find(input.cameraId);
  if (it == entries_.end())
    co_return written;
  it->second.writing = false;
  if (written)
  {
    it->second.persisted = input.state;
    it->second.persistedObeyed = input.obeyed;
    co_return true;
  }
  if (it->second.state == input.state && it->second.persisted != input.state &&
      it->second.confirmed != CameraPrivacyState::On)
  {
    it->second.state = it->second.persisted;
    it->second.confirmed = it->second.persisted;
  }
  co_return false;
}

drogon::Task<PrivacyTransition> PrivacyStateService::refresh(
    PrivacyRefreshInput input)
{
  PrivacyTransition transition;
  const int64_t cameraId = input.camera.id;
  if (cameraId <= 0)
    co_return transition;
  if (!input.readable)
  {
    transition.before = stateOf(cameraId);
    transition.after = transition.before;
    co_return transition;
  }

  const CameraPrivacyState observed = observedState(input.driverStatus);
  bool persist = false;
  bool noticed = false;
  bool obeyedBefore = false;
  bool obeyedNow = false;
  bool persistedObeyedBefore = false;
  CameraPrivacyState persistedStateBefore = CameraPrivacyState::Unknown;
  {
    std::scoped_lock lock(mutex_);
    auto& entry = entries_[cameraId];
    transition.before = entry.state;
    obeyedBefore = obeyed(entry);
    entry.state = observed;
    if (observed != CameraPrivacyState::Unknown)
      entry.confirmed = observed;
    if (doubted(entry))
    {
      if (!entry.noticed)
      {
        entry.noticed = true;
        noticed = true;
      }
    }
    else
    {
      entry.noticed = false;
    }
    obeyedNow = obeyed(entry);
    persistedObeyedBefore = entry.persistedObeyed;
    persistedStateBefore = entry.persisted;
    persist = (observed != entry.persisted ||
               obeyedNow != entry.persistedObeyed) &&
              !entry.writing;
    if (persist)
      entry.writing = true;
  }

  bool written = true;
  if (persist)
    written = co_await persistState({.cameraId = cameraId,
                                     .state = observed,
                                     .obeyed = obeyedNow});
  const CameraPrivacyState after = written ? observed : stateOf(cameraId);
  bool obeyedAfter = false;
  {
    std::scoped_lock lock(mutex_);
    const auto it = entries_.find(cameraId);
    obeyedAfter = it != entries_.end() && obeyed(it->second);
  }
  transition.after = after;
  transition.changed = transition.before != after;

  if (obeyedAfter && !obeyedBefore)
  {
    SnapshotStore::instance().forget(cameraId);
    StreamHub::instance().dropCachedFrames(cameraId);
    LOG_WARN << "Camera privacy: camera " << cameraId
             << " is lens-masked; Argus stops seeing, hearing and recording it";
  }
  if (noticed)
  {
    if (const auto* notifier = privacy_notice::getNotifier())
    {
      co_await notifier->doubtful({.cameraId = cameraId,
                                   .cameraName = input.camera.name,
                                   .atMs = epochNowMs()});
    }
  }
  if ((!transition.changed && !persist) || !written)
    co_return transition;

  if (const auto* sink = camera_change::getSink())
  {
    Json::Value before = input.camera.toJson();
    before["privacyState"] = cameraPrivacyStateToString(persistedStateBefore);
    before["privacyObeyed"] = persistedObeyedBefore;
    Json::Value row = before;
    row["privacyState"] = cameraPrivacyStateToString(after);
    row["privacyObeyed"] = obeyedNow;
    co_await sink->publishAudit({.recordId = cameraId,
                                 .tableName = TableName::Camera,
                                 .before = before,
                                 .after = row,
                                 .actorId = std::nullopt,
                                 .client = nullptr});
  }
  if (obeyedBefore && !obeyedAfter)
  {
    CameraSceneLog::instance().noteAimed(cameraId, epochNowMs());
    LOG_INFO << "Camera privacy: camera " << cameraId
             << " is unmasked; Argus sees it again";
  }
  if (const auto* changes = camera_change::getSink())
  {
    co_await changes->publishResource({.scope = ResourceScope::Status,
                                       .ids = {cameraId},
                                       .user = 0,
                                       .client = nullptr});
    co_await changes->publishResource({.scope = ResourceScope::Overview,
                                       .ids = {},
                                       .user = 0,
                                       .client = nullptr});
  }
  co_return transition;
}
