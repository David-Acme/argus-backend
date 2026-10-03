#include "camera-scene-log.hxx"

CameraSceneLog& CameraSceneLog::instance()
{
  static CameraSceneLog log;
  return log;
}

void CameraSceneLog::noteAimed(int64_t cameraId, int64_t atMs)
{
  std::scoped_lock lock(mutex_);
  scenes_[cameraId].aimedAtMs = atMs;
}

void CameraSceneLog::notePrivacy(int64_t cameraId, bool enabled)
{
  std::scoped_lock lock(mutex_);
  scenes_[cameraId].privacy = enabled;
}

CameraScene CameraSceneLog::sceneOf(int64_t cameraId) const
{
  std::scoped_lock lock(mutex_);
  const auto it = scenes_.find(cameraId);
  return it == scenes_.end() ? CameraScene{} : it->second;
}

void CameraSceneLog::forget(int64_t cameraId)
{
  std::scoped_lock lock(mutex_);
  scenes_.erase(cameraId);
}
