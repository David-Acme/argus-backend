#include "patrol-service.hxx"

#include <feature/camera-control/services/aim-board.hxx>
#include <feature/camera-control/services/ptz-lease.hxx>
#include <feature/look/services/follow-service.hxx>
#include <shared/services/privacy-state/privacy-state.hxx>
#include <shared/utils/in-flight/in-flight.hxx>

#include <chrono>
#include <drogon/drogon.h>
#include <trantor/utils/Logger.h>
#include <utility>

namespace
{
constexpr double kTickSeconds = 30.0;
constexpr double kFollowSweepSeconds = 1.0;

int64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
}

LookableCameras& LookableCameras::instance()
{
  static LookableCameras cameras;
  return cameras;
}

void LookableCameras::replace(std::unordered_map<int64_t, AimSettings> settings)
{
  const std::scoped_lock lock(mutex_);
  settings_ = std::move(settings);
}

std::optional<AimSettings> LookableCameras::of(int64_t cameraId)
{
  const std::scoped_lock lock(mutex_);
  const auto found = settings_.find(cameraId);
  if (found == settings_.end())
    return std::nullopt;
  return found->second;
}

bool LookableCameras::lookable(int64_t cameraId)
{
  const auto settings = of(cameraId);
  return settings && settings->lookable();
}

namespace patrol
{
bool due(const PatrolDueInput& input)
{
  return input.enabled && input.awayOrArmed && input.cameraPatrols && input.leaseFree &&
         input.nowMs - input.lastTourMs >= input.everyMs;
}
}

PatrolService::PatrolService(PatrolDependencies dependencies) : dependencies_(std::move(dependencies)) {}

void PatrolService::start()
{
  if (running_.exchange(true))
    return;
  drogon::app().getLoop()->runEvery(kFollowSweepSeconds, [this] {
    if (running_.load())
      FollowService::sweep();
  });
  drogon::app().getLoop()->runEvery(kTickSeconds, [this] {
    if (!running_.load())
      return;
    drogon::async_run([this]() -> drogon::Task<void> {
      const in_flight::Guard guard(inFlight_);
      try {
        co_await tick();
      }
      catch (const std::exception& error) {
        LOG_WARN << "Camera patrol: tick failed: " << error.what();
      }
      co_return;
    });
  });
}

void PatrolService::requestStop()
{
  running_.store(false);
}

bool PatrolService::drained() const
{
  return inFlight_.load() == 0;
}

drogon::Task<void> PatrolService::tick()
{
  std::unordered_map<int64_t, AimSettings> settings;
  for (const CameraSchema& camera : co_await repository_.findEnabled())
    settings[camera.id] = aim_settings::parse({.capabilities = camera.capabilities, .config = camera.config});
  LookableCameras::instance().replace(settings);
  const CameraPtzConfig& ptz = CameraControlFeatureService::ptz();
  if (!ptz.patrolEnabled || (dependencies_.active && !dependencies_.active()))
    co_return;
  const bool awayOrArmed = dependencies_.awayOrArmed && dependencies_.awayOrArmed();
  const int64_t now = nowMs();
  for (const auto& [cameraId, aim] : settings) {
    int64_t lastTour = 0;
    {
      const std::scoped_lock lock(toursMutex_);
      lastTour = lastTourMs_[cameraId];
    }
    const PtzLeaseState lease = PtzLease::instance().stateOf({.cameraId = cameraId, .atMs = now});
    if (!patrol::due({.enabled = ptz.patrolEnabled,
                      .awayOrArmed = awayOrArmed,
                      .cameraPatrols = aim.patrol && aim.lookable() && !aim.patrolPresets.empty(),
                      .leaseFree = lease.holder == PtzHolder::None && now >= lease.parkUntilMs,
                      .lastTourMs = lastTour,
                      .nowMs = now,
                      .everyMs = ptz.patrolEveryMs}) ||
        PrivacyStateService::instance().masked(cameraId) || FollowBoard::instance().following(cameraId))
      continue;
    {
      const std::scoped_lock lock(toursMutex_);
      lastTourMs_[cameraId] = now;
    }
    co_await tour(cameraId, aim);
  }
}

drogon::Task<void> PatrolService::tour(int64_t cameraId, AimSettings settings)
{
  const CameraPtzConfig& ptz = CameraControlFeatureService::ptz();
  LOG_INFO << "Camera patrol: camera " << cameraId << " tours " << settings.patrolPresets.size() << " preset(s)";
  for (const std::string& preset : settings.patrolPresets) {
    if (!running_.load() || FollowBoard::instance().following(cameraId))
      co_return;
    const PtzLeaseState lease = PtzLease::instance().stateOf({.cameraId = cameraId, .atMs = nowMs()});
    if (lease.holder == PtzHolder::Viewer || lease.holder == PtzHolder::Person) {
      LOG_INFO << "Camera patrol: camera " << cameraId << " pauses its tour; someone holds the controls";
      co_return;
    }
    if (!AimBoard::instance().wearAllows(
            {.cameraId = cameraId, .nowMs = nowMs(), .perMinute = ptz.movesPerMinute, .dailyCap = ptz.dailyMoveCap}))
      break;
    const auto moved = co_await control_.gotoByArgus({.cameraId = cameraId, .presetId = preset, .reason = "patrol"});
    if (!moved || !moved->ok)
      break;
    co_await drogon::sleepCoro(drogon::app().getLoop(), std::chrono::milliseconds(ptz.settleMs + ptz.patrolDwellMs));
  }
  if (!FollowBoard::instance().following(cameraId))
    static_cast<void>(co_await control_.returnHome({.cameraId = cameraId, .reason = "patrol"}));
}
