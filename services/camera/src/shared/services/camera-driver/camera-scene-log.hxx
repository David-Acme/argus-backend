#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>

struct CameraScene
{
  int64_t aimedAtMs{0};
  int64_t moveStartMs{0};
  int64_t moveExpectedEndMs{0};
  int64_t moveEndedAtMs{0};
  bool returnsHome{false};

  [[nodiscard]] bool moving(int64_t atMs) const
  {
    return moveStartMs > 0 && moveEndedAtMs == 0 && atMs >= moveStartMs;
  }
};

struct CameraMoveStart
{
  int64_t cameraId{0};
  int64_t startMs{0};
  int64_t expectedEndMs{0};
  bool returnsHome{false};
};

struct CameraMoveEnd
{
  int64_t cameraId{0};
  int64_t atMs{0};
};

class CameraSceneLog
{
public:
  static CameraSceneLog& instance();

  void noteAimed(int64_t cameraId, int64_t atMs);
  void beginMove(const CameraMoveStart& move);
  void endMove(const CameraMoveEnd& move);
  [[nodiscard]] CameraScene sceneOf(int64_t cameraId) const;
  [[nodiscard]] bool moving(int64_t cameraId, int64_t atMs) const;
  void forget(int64_t cameraId);

private:
  mutable std::mutex mutex_;
  std::unordered_map<int64_t, CameraScene> scenes_;
};
