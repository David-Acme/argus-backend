#include "follow-service.hxx"

#include <feature/camera-control/services/aim-board.hxx>
#include <feature/camera-control/services/camera-control-feature-service.hxx>

#include <chrono>
#include <cmath>
#include <drogon/drogon.h>
#include <span>
#include <trantor/utils/Logger.h>

namespace
{
constexpr size_t kMaxSamples = 8;
constexpr double kReacquireRadius = 0.25;

int64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
}

FollowBoard& FollowBoard::instance()
{
  static FollowBoard board;
  return board;
}

void FollowBoard::start(const FollowStart& start)
{
  const std::scoped_lock lock(mutex_);
  sessions_[start.cameraId] = {.trackId = start.trackId,
                               .startedMs = start.atMs,
                               .lastSeenMs = start.atMs,
                               .moveInFlight = false,
                               .reacquire = true,
                               .samples = {}};
  AimBoard::instance().setActivity({.cameraId = start.cameraId, .activity = AimActivity::Following});
}

bool FollowBoard::following(int64_t cameraId)
{
  const std::scoped_lock lock(mutex_);
  return sessions_.contains(cameraId);
}

std::optional<FollowStep> FollowBoard::sighted(const FollowStepInput& input)
{
  const TrackSighting& sighting = input.sighting;
  const std::scoped_lock lock(mutex_);
  const auto found = sessions_.find(sighting.cameraId);
  if (found == sessions_.end())
    return std::nullopt;
  Session& session = found->second;
  const double cx = sighting.box.x + sighting.box.w / 2.0;
  const double cy = sighting.box.y + sighting.box.h / 2.0;
  if (sighting.trackId != session.trackId) {
    if (!session.reacquire || std::hypot(cx - 0.5, cy - 0.5) > kReacquireRadius)
      return std::nullopt;
    session.trackId = sighting.trackId;
    session.samples.clear();
  }
  session.reacquire = false;
  session.lastSeenMs = sighting.atMs;
  session.samples.push_back({.cx = cx, .cy = cy, .atMs = sighting.atMs});
  while (session.samples.size() > kMaxSamples)
    session.samples.pop_front();
  if (session.moveInFlight)
    return std::nullopt;
  const std::vector<TrackSample> samples(session.samples.begin(), session.samples.end());
  NormalizedBox target = sighting.box;
  if (const auto ahead = aim_math::lead(
          {.samples = samples, .minSamples = input.leadMinSamples, .consistency = input.leadConsistency,
           .aheadMs = 1500}))
    target = {.x = ahead->x - sighting.box.w / 2.0, .y = ahead->y - sighting.box.h / 2.0, .w = sighting.box.w,
              .h = sighting.box.h};
  const auto offset = aim_math::centerOffset({.box = target, .fov = input.fov, .deadZone = input.deadZone});
  if (!offset)
    return std::nullopt;
  session.moveInFlight = true;
  return FollowStep{.cameraId = sighting.cameraId, .delta = *offset};
}

void FollowBoard::settled(int64_t cameraId)
{
  const std::scoped_lock lock(mutex_);
  const auto found = sessions_.find(cameraId);
  if (found == sessions_.end())
    return;
  found->second.moveInFlight = false;
  found->second.reacquire = true;
  found->second.samples.clear();
}

std::vector<int64_t> FollowBoard::lost(const FollowLossQuery& query)
{
  const int64_t now = query.nowMs;
  const int64_t lostAfterMs = query.lostAfterMs;
  std::vector<int64_t> ended;
  const std::scoped_lock lock(mutex_);
  for (auto it = sessions_.begin(); it != sessions_.end();) {
    if (!it->second.moveInFlight && now - it->second.lastSeenMs >= lostAfterMs) {
      ended.push_back(it->first);
      it = sessions_.erase(it);
    }
    else {
      ++it;
    }
  }
  return ended;
}

void FollowBoard::stop(int64_t cameraId)
{
  const std::scoped_lock lock(mutex_);
  sessions_.erase(cameraId);
}

void FollowService::sighting(const TrackSighting& sighting)
{
  const CameraPtzConfig& ptz = CameraControlFeatureService::ptz();
  const auto step = FollowBoard::instance().sighted(
      {.sighting = sighting,
       .fov = {.horizontalDeg = ptz.fovHorizontalDeg, .verticalDeg = ptz.fovVerticalDeg},
       .deadZone = ptz.deadZone,
       .leadMinSamples = ptz.leadMinSamples,
       .leadConsistency = ptz.leadConsistency});
  if (!step || !drogon::app().isRunning())
    return;
  const int64_t at = nowMs();
  const AimSnapshot aim = AimBoard::instance().snapshot(step->cameraId);
  if (!AimBoard::instance().wearAllows(
          {.cameraId = step->cameraId, .nowMs = at, .perMinute = ptz.movesPerMinute, .dailyCap = ptz.dailyMoveCap}) ||
      !aim.known) {
    FollowBoard::instance().settled(step->cameraId);
    return;
  }
  const FollowStep move = *step;
  drogon::app().getLoop()->queueInLoop([move, settleMs = ptz.settleMs] {
    drogon::async_run([move, settleMs]() -> drogon::Task<void> {
      const CameraControlFeatureService control;
      const auto result = co_await control.aimByArgus(
          {.cameraId = move.cameraId, .delta = move.delta, .reason = "follow", .activity = AimActivity::Following});
      if (!result || !result->ok) {
        LOG_INFO << "Camera follow: camera " << move.cameraId << " stops following ("
                 << (result ? result->failure : std::string("camera not found")) << ")";
        FollowBoard::instance().stop(move.cameraId);
        co_return;
      }
      co_await drogon::sleepCoro(drogon::app().getLoop(), std::chrono::milliseconds(settleMs));
      FollowBoard::instance().settled(move.cameraId);
    });
  });
}

void FollowService::sweep()
{
  const CameraPtzConfig& ptz = CameraControlFeatureService::ptz();
  for (const int64_t cameraId : FollowBoard::instance().lost({.nowMs = nowMs(), .lostAfterMs = ptz.followLostMs})) {
    LOG_INFO << "Camera follow: camera " << cameraId << " lost its target; it returns home in "
             << ptz.returnAfterFollowMs / 1000 << " s";
    AimBoard::instance().setActivity({.cameraId = cameraId, .activity = AimActivity::Idle});
    if (!drogon::app().isRunning())
      continue;
    drogon::app().getLoop()->runAfter(static_cast<double>(ptz.returnAfterFollowMs) / 1000.0, [cameraId] {
      if (FollowBoard::instance().following(cameraId))
        return;
      drogon::async_run([cameraId]() -> drogon::Task<void> {
        const CameraControlFeatureService control;
        static_cast<void>(co_await control.returnHome({.cameraId = cameraId, .reason = "follow"}));
      });
    });
  }
}
