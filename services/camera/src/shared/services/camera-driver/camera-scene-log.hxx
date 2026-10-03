#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>

struct CameraScene
{
  int64_t aimedAtMs{0};
  bool privacy{false};
};

class CameraSceneLog
{
public:
  static CameraSceneLog& instance();

  void noteAimed(int64_t cameraId, int64_t atMs);
  void notePrivacy(int64_t cameraId, bool enabled);
  [[nodiscard]] CameraScene sceneOf(int64_t cameraId) const;
  void forget(int64_t cameraId);

private:
  mutable std::mutex mutex_;
  std::unordered_map<int64_t, CameraScene> scenes_;
};
