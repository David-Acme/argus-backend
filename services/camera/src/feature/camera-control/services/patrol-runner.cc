#include "patrol-runner.hxx"

#include <feature/camera-control/services/aim-board.hxx>
#include <shared/services/privacy-state/privacy-state.hxx>
#include <shared/utils/in-flight/in-flight.hxx>

#include <algorithm>
#include <chrono>
#include <ctime>
#include <drogon/HttpAppFramework.h>
#include <exception>
#include <string>
#include <utility>

namespace
{
int64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

int localHourNow()
{
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  if (localtime_r(&now, &local) == nullptr)
    return 0;
  return local.tm_hour;
}

std::string endReason(const PatrolSituation& situation, bool configured)
{
  if (!configured)
    return "removed";
  if (!situation.enabled)
    return "disabled";
  if (situation.stopCount == 0)
    return "no_stops";
  if (!situation.moduleActive)
    return "module_off";
  if (!situation.allowed)
    return "not_away";
  if (!situation.online)
    return "offline";
  if (situation.masked)
    return "privacy";
  return "window_closed";
}
}

PatrolRunner::PatrolRunner(PatrolRunnerConfig config) : config_(std::move(config))
{
}

void PatrolRunner::start()
{
  if (running_.exchange(true))
    return;
  timer_ = drogon::app().getLoop()->runEvery(config_.tickSeconds, [this] {
    if (!running_.load(std::memory_order_acquire) || ticking_.exchange(true))
      return;
    drogon::async_run([this]() -> drogon::Task<void> {
      try {
        co_await tick();
      }
      catch (const std::exception& error) {
        LOG_WARN << "Camera patrol: tick failed: " << error.what();
      }
      catch (...) {
        LOG_WARN << "Camera patrol: tick failed with unknown error";
      }
      ticking_.store(false, std::memory_order_release);
    });
  });
  LOG_INFO << "Camera patrol: checking the patrols every " << config_.tickSeconds << " s";
}

void PatrolRunner::requestStop()
{
  if (!running_.exchange(false))
    return;
  if (timer_) {
    drogon::app().getLoop()->invalidateTimer(*timer_);
    timer_.reset();
  }
}

bool PatrolRunner::drained() const
{
  return inFlight_.load(std::memory_order_acquire) == 0;
}

PatrolProgress PatrolRunner::progressOf(int64_t cameraId)
{
  const std::scoped_lock lock(mutex_);
  const auto found = progress_.find(cameraId);
  return found == progress_.end() ? PatrolProgress{} : found->second;
}

void PatrolRunner::keep(int64_t cameraId, const PatrolProgress& progress)
{
  const std::scoped_lock lock(mutex_);
  if (!progress.active && progress.nextStop == 0 && progress.restUntilMs == 0) {
    progress_.erase(cameraId);
    return;
  }
  progress_[cameraId] = progress;
}

std::vector<int64_t> PatrolRunner::activeCameras()
{
  const std::scoped_lock lock(mutex_);
  std::vector<int64_t> cameras;
  cameras.reserve(progress_.size());
  for (const auto& [cameraId, progress] : progress_)
    if (progress.active)
      cameras.push_back(cameraId);
  return cameras;
}

void PatrolRunner::prune(const std::vector<int64_t>& seen)
{
  const std::scoped_lock lock(mutex_);
  std::erase_if(progress_, [&seen](const auto& entry) {
    return !entry.second.active && std::ranges::find(seen, entry.first) == seen.end();
  });
}

drogon::Task<void> PatrolRunner::tick()
{
  const in_flight::Guard guard(inFlight_);
  if (!running_.load(std::memory_order_acquire))
    co_return;
  const CameraPtzConfig& ptz = CameraControlFeatureService::ptz();
  const PatrolClock moment{.nowMs = nowMs(),
                          .localHour = localHourNow(),
                          .moduleActive = !config_.active || config_.active(),
                          .allowed = ptz.patrolEnabled && config_.awayOrArmed && config_.awayOrArmed(),
                          .everyMs = ptz.patrolEveryMs};
  const auto patrols = co_await repository_.findActive();
  std::vector<int64_t> seen;
  seen.reserve(patrols.size());
  for (const auto& active : patrols) {
    seen.push_back(active.patrol.cameraId);
    co_await follow({.cameraId = active.patrol.cameraId, .patrol = active.patrol, .online = active.cameraOnline},
                    moment);
  }
  for (const int64_t cameraId : activeCameras())
    if (std::ranges::find(seen, cameraId) == seen.end())
      co_await follow({.cameraId = cameraId, .patrol = std::nullopt, .online = true}, moment);
  prune(seen);
}

drogon::Task<void> PatrolRunner::follow(PatrolTarget target, PatrolClock moment)
{
  const int64_t cameraId = target.cameraId;
  PatrolProgress progress = progressOf(cameraId);
  if (config_.busy && config_.busy(cameraId)) {
    if (progress.active) {
      progress.active = false;
      keep(cameraId, progress);
      LOG_INFO << "Camera " << cameraId << ": the patrol yields to a follow";
    }
    co_return;
  }
  const PatrolSituation situation{
      .enabled = target.patrol.has_value() && target.patrol->isEnabled,
      .stopCount = target.patrol ? target.patrol->stops.size() : 0,
      .dwellMs = target.patrol ? target.patrol->dwellSeconds * 1000 : 0,
      .window = target.patrol ? target.patrol->window : std::nullopt,
      .localHour = moment.localHour,
      .online = target.online,
      .masked = PrivacyStateService::instance().masked(cameraId),
      .moduleActive = moment.moduleActive,
      .allowed = moment.allowed,
      .everyMs = moment.everyMs,
      .lease = PtzLease::instance().stateOf({.cameraId = cameraId, .atMs = moment.nowMs}),
      .nowMs = moment.nowMs,
      .progress = progress};
  const PatrolStep step = patrol_plan::decide(situation);

  switch (step.kind) {
    case PatrolStepKind::Idle:
    case PatrolStepKind::Wait:
      co_return;
    case PatrolStepKind::Yield:
      if (progress.active) {
        progress.active = false;
        keep(cameraId, progress);
        LOG_INFO << "Camera " << cameraId << ": the patrol yields to whoever aims the camera";
      }
      co_return;
    case PatrolStepKind::Rest: {
      keep(cameraId,
           {.active = false,
            .nextStop = 0,
            .arrivedAtMs = 0,
            .lapStartedMs = progress.lapStartedMs,
            .restUntilMs = progress.lapStartedMs + moment.everyMs});
      co_await service_.endPatrol({.cameraId = cameraId, .returnHome = true});
      co_await service_.recordPatrol({.cameraId = cameraId, .phase = "ended", .reason = "lap"});
      LOG_INFO << "Camera " << cameraId << ": the patrol finished a lap and rests at home";
      co_return;
    }
    case PatrolStepKind::End: {
      keep(cameraId, PatrolProgress{});
      const std::string reason = endReason(situation, target.patrol.has_value());
      co_await service_.endPatrol({.cameraId = cameraId, .returnHome = step.returnHome});
      co_await service_.recordPatrol({.cameraId = cameraId, .phase = "ended", .reason = reason});
      LOG_INFO << "Camera " << cameraId << ": the patrol ended (" << reason << ")";
      co_return;
    }
    case PatrolStepKind::Advance:
      break;
  }

  if (!target.patrol || target.patrol->stops.empty())
    co_return;
  const std::size_t count = target.patrol->stops.size();
  const std::string presetId = target.patrol->stops[step.stop % count];
  const bool starting = !progress.active;
  const CameraPtzConfig& ptz = CameraControlFeatureService::ptz();
  if (!AimBoard::instance().wearAllows(
          {.cameraId = cameraId, .nowMs = moment.nowMs, .perMinute = ptz.movesPerMinute, .dailyCap = ptz.dailyMoveCap}))
    co_return;
  const auto result =
      co_await service_.gotoByArgus({.cameraId = cameraId, .presetId = presetId, .reason = "patrol"});
  const int64_t movedAt = nowMs();
  const int64_t lapStartedMs = step.stop == 0 ? movedAt : progress.lapStartedMs;

  if (result && result->ok) {
    keep(cameraId,
         {.active = true,
          .nextStop = (step.stop + 1) % count,
          .arrivedAtMs = movedAt + config_.settleMs,
          .lapStartedMs = lapStartedMs,
          .restUntilMs = 0});
    if (starting)
      co_await service_.recordPatrol({.cameraId = cameraId, .phase = "started", .reason = "schedule"});
    co_return;
  }
  const std::string failure = result ? result->failure : std::string("not_found");
  if (failure == "lease_held" || failure == "privacy_on") {
    keep(cameraId,
         {.active = false,
          .nextStop = progress.nextStop,
          .arrivedAtMs = progress.arrivedAtMs,
          .lapStartedMs = progress.lapStartedMs,
          .restUntilMs = 0});
    co_return;
  }
  const bool holds =
      PtzLease::instance().holds({.cameraId = cameraId, .holder = PtzHolder::Argus, .userId = 0, .atMs = movedAt});
  keep(cameraId,
       {.active = holds,
        .nextStop = (step.stop + 1) % count,
        .arrivedAtMs = movedAt,
        .lapStartedMs = lapStartedMs,
        .restUntilMs = 0});
  LOG_WARN << "Camera " << cameraId << ": the patrol could not reach preset " << presetId << " ("
           << (result ? result->error : std::string("camera not found")) << "); it moves on";
  if (starting && holds)
    co_await service_.recordPatrol({.cameraId = cameraId, .phase = "started", .reason = "schedule"});
}
