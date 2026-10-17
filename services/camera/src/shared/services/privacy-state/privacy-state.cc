#include "privacy-state.hxx"

#include <algorithm>
#include <chrono>
#include <drogon/HttpAppFramework.h>
#include <drogon/drogon.h>
#include <shared/repositories/camera/camera-repository.hxx>
#include <shared/services/camera-driver/camera-scene-log.hxx>
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
}

PrivacyStateService& PrivacyStateService::instance()
{
  static PrivacyStateService service;
  return service;
}

void PrivacyStateService::seed(const std::vector<CameraSchema>& cameras)
{
  for (const auto& camera : cameras)
  {
    if (camera.id <= 0)
      continue;
    CameraSceneLog::instance().notePrivacy(
        camera.id, cameraPrivacyMasked(camera.privacyState));
  }
}

CameraPrivacyState PrivacyStateService::stateOf(int64_t cameraId) const
{
  return masked(cameraId) ? CameraPrivacyState::On : CameraPrivacyState::Off;
}

bool PrivacyStateService::masked(int64_t cameraId) const
{
  return CameraSceneLog::instance().sceneOf(cameraId).privacy;
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
  CameraSceneLog::instance().forget(cameraId);
}

drogon::Task<PrivacyTransition> PrivacyStateService::refresh(
    PrivacyRefreshInput input)
{
  PrivacyTransition transition;
  if (input.camera.id <= 0)
    co_return transition;
  transition.before = stateOf(input.camera.id);
  transition.after = transition.before;
  if (!input.readable)
    co_return transition;
  CameraPrivacyState after = CameraPrivacyState::Unknown;
  const Json::Value& enabled = input.driverStatus[kPrivacyEnabledKey];
  if (enabled.isBool())
    after = enabled.asBool() ? CameraPrivacyState::On : CameraPrivacyState::Off;
  transition.after = after;
  transition.changed = after != transition.before;
  if (!transition.changed)
    co_return transition;
  if (drogon::app().isRunning())
  {
    try
    {
      CameraRepository repository;
      static_cast<void>(co_await repository.update(
          input.camera.id, {.privacyState = after, .client = nullptr}));
    }
    catch (const std::exception& error)
    {
      LOG_WARN << "Camera privacy: the state of camera " << input.camera.id
               << " could not be persisted: " << error.what();
    }
  }
  if (const auto* sink = camera_change::getSink())
  {
    Json::Value before = input.camera.toJson();
    before["privacyState"] = cameraPrivacyStateToString(transition.before);
    Json::Value row = before;
    row["privacyState"] = cameraPrivacyStateToString(after);
    co_await sink->publishAudit({.recordId = input.camera.id,
                                 .tableName = TableName::Camera,
                                 .before = before,
                                 .after = row,
                                 .actorId = std::nullopt,
                                 .client = nullptr});
  }
  CameraSceneLog::instance().notePrivacy(input.camera.id,
                                         after == CameraPrivacyState::On);
  if (after == CameraPrivacyState::On)
  {
    SnapshotStore::instance().forget(input.camera.id);
    StreamHub::instance().dropCachedFrames(input.camera.id);
    LOG_WARN << "Camera privacy: camera " << input.camera.id
             << " is lens-masked; Argus stops seeing, hearing and recording it";
  }
  else if (transition.before == CameraPrivacyState::On)
  {
    CameraSceneLog::instance().noteAimed(input.camera.id, epochNowMs());
    LOG_INFO << "Camera privacy: camera " << input.camera.id
             << " is unmasked; Argus sees it again";
  }
  if (const auto* changes = camera_change::getSink())
  {
    co_await changes->publishResource({.scope = ResourceScope::Status,
                                       .ids = {input.camera.id},
                                       .user = 0,
                                       .client = nullptr});
    co_await changes->publishResource({.scope = ResourceScope::Overview,
                                       .ids = {},
                                       .user = 0,
                                       .client = nullptr});
  }
  co_return transition;
}
