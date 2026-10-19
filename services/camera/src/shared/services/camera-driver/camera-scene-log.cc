#include "camera-scene-log.hxx"

CameraSceneLog& CameraSceneLog::instance()
{
  static CameraSceneLog log;
  return log;
}

void CameraSceneLog::noteAimed(const CameraInstant& aimed)
{
  std::scoped_lock lock(mutex_);
  CameraScene& scene = scenes_[aimed.cameraId];
  scene.aimedAtMs = aimed.atMs;
  scene.returnsHome = false;
  scene.argusAway = false;
}

void CameraSceneLog::setArgusAway(const CameraAway& away)
{
  std::scoped_lock lock(mutex_);
  scenes_[away.cameraId].argusAway = away.away;
}

void CameraSceneLog::beginMove(const CameraMoveStart& move)
{
  std::scoped_lock lock(mutex_);
  CameraScene& scene = scenes_[move.cameraId];
  scene.moveStartMs = move.startMs;
  scene.moveExpectedEndMs = move.expectedEndMs;
  scene.moveEndedAtMs = 0;
  scene.returnsHome = move.returnsHome;
}

void CameraSceneLog::endMove(const CameraMoveEnd& move)
{
  std::scoped_lock lock(mutex_);
  CameraScene& scene = scenes_[move.cameraId];
  if (scene.moveStartMs == 0)
    return;
  scene.moveEndedAtMs = move.atMs;
  if (!scene.returnsHome)
    scene.aimedAtMs = move.atMs;
}

CameraScene CameraSceneLog::sceneOf(int64_t cameraId) const
{
  std::scoped_lock lock(mutex_);
  const auto it = scenes_.find(cameraId);
  return it == scenes_.end() ? CameraScene{} : it->second;
}

bool CameraSceneLog::moving(const CameraInstant& instant) const
{
  std::scoped_lock lock(mutex_);
  const auto it = scenes_.find(instant.cameraId);
  return it != scenes_.end() && it->second.moving(instant.atMs);
}

void CameraSceneLog::forget(int64_t cameraId)
{
  std::scoped_lock lock(mutex_);
  scenes_.erase(cameraId);
}
