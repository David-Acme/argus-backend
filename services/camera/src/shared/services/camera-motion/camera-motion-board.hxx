#pragma once

#include <cstdint>
#include <mutex>
#include <unordered_map>

struct CameraMotionReport
{
  int64_t cameraId{0};
  bool moving{false};
  int64_t atMs{0};
};

class CameraMotionBoard
{
public:
  static constexpr int64_t kHoldMs = 5000;
  static constexpr int64_t kStuckMs = 600000;

  void report(const CameraMotionReport& report);
  [[nodiscard]] bool raisesCadence(int64_t cameraId, int64_t nowMs);
  void forget(int64_t cameraId);
  [[nodiscard]] int64_t mutedEpisodes() const;

private:
  struct Episode
  {
    bool active{false};
    bool moving{false};
    bool muted{false};
    int64_t startedMs{0};
    int64_t endedMs{0};
  };

  [[nodiscard]] static bool quietPastHold(const Episode& episode, int64_t nowMs);

  mutable std::mutex mutex_;
  std::unordered_map<int64_t, Episode> cameras_;
  int64_t mutedEpisodes_{0};
};
