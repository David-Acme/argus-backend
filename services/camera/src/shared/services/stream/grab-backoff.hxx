#pragma once

#include <array>
#include <cstdint>
#include <mutex>
#include <unordered_map>

struct GrabPoint
{
  int64_t cameraId{0};
  int64_t nowMs{0};
};

struct GrabAttempt
{
  bool attempt{true};
  bool firstFailure{false};
  int64_t retryDelayMs{0};
};

class GrabBackoff
{
public:
  static constexpr std::array<int64_t, 7> kRetryDelaysMs{1000, 2000, 4000,
                                                         8000, 16000, 30000, 30000};

  [[nodiscard]] GrabAttempt begin(const GrabPoint& point) const;
  void noteFailure(const GrabPoint& point);
  void noteSuccess(int64_t cameraId);

private:
  struct Streak
  {
    int consecutive{0};
    int64_t nextAttemptAtMs{0};
  };

  [[nodiscard]] static int64_t delayFor(int consecutive);

  mutable std::mutex mutex_;
  std::unordered_map<int64_t, Streak> streaks_;
};
