#include <shared/services/camera-motion/camera-motion-board.hxx>

#include <trantor/utils/Logger.h>

bool CameraMotionBoard::quietPastHold(const Episode& episode, int64_t nowMs)
{
  return !episode.moving && nowMs - episode.endedMs >= kHoldMs;
}

void CameraMotionBoard::report(const CameraMotionReport& report)
{
  const std::scoped_lock lock(mutex_);
  if (!report.moving) {
    const auto found = cameras_.find(report.cameraId);
    if (found != cameras_.end() && found->second.active && found->second.moving) {
      found->second.moving = false;
      found->second.endedMs = report.atMs;
    }
    return;
  }
  Episode& episode = cameras_[report.cameraId];
  if (episode.moving)
    return;
  if (!episode.active || quietPastHold(episode, report.atMs))
    episode = {.active = true, .moving = true, .muted = false, .startedMs = report.atMs, .endedMs = 0};
  else
    episode.moving = true;
}

bool CameraMotionBoard::raisesCadence(int64_t cameraId, int64_t nowMs)
{
  const std::scoped_lock lock(mutex_);
  const auto found = cameras_.find(cameraId);
  if (found == cameras_.end() || !found->second.active)
    return false;
  Episode& episode = found->second;
  if (quietPastHold(episode, nowMs)) {
    episode = {};
    return false;
  }
  if (episode.muted)
    return false;
  if (nowMs - episode.startedMs > kStuckMs) {
    episode.muted = true;
    ++mutedEpisodes_;
    LOG_WARN << "Camera motion: camera " << cameraId
             << " has reported motion for more than 10 minutes; its events stop raising the cadence until it "
                "reports the end and stays quiet for "
             << kHoldMs / 1000 << " s";
    return false;
  }
  return true;
}

void CameraMotionBoard::forget(int64_t cameraId)
{
  const std::scoped_lock lock(mutex_);
  cameras_.erase(cameraId);
}

int64_t CameraMotionBoard::mutedEpisodes() const
{
  const std::scoped_lock lock(mutex_);
  return mutedEpisodes_;
}
