#include "grab-backoff.hxx"

#include <algorithm>
#include <cstddef>

int64_t GrabBackoff::delayFor(int consecutive)
{
  const std::size_t step = static_cast<std::size_t>(
      std::clamp(consecutive, 1, static_cast<int>(kRetryDelaysMs.size())));
  return kRetryDelaysMs[step - 1];
}

GrabAttempt GrabBackoff::begin(const GrabPoint& point) const
{
  std::scoped_lock lock(mutex_);
  const auto found = streaks_.find(point.cameraId);
  if (found != streaks_.end() && point.nowMs < found->second.nextAttemptAtMs)
    return {.attempt = false, .firstFailure = false, .retryDelayMs = 0};
  const int consecutive = found == streaks_.end() ? 0 : found->second.consecutive;
  return {.attempt = true,
          .firstFailure = consecutive == 0,
          .retryDelayMs = delayFor(consecutive + 1)};
}

void GrabBackoff::noteFailure(const GrabPoint& point)
{
  std::scoped_lock lock(mutex_);
  Streak& streak = streaks_[point.cameraId];
  ++streak.consecutive;
  streak.nextAttemptAtMs = point.nowMs + delayFor(streak.consecutive);
}

void GrabBackoff::noteSuccess(int64_t cameraId)
{
  std::scoped_lock lock(mutex_);
  streaks_.erase(cameraId);
}
