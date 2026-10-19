#include "look-refresh.hxx"

#include <feature/look/services/follow-service.hxx>
#include <shared/utils/in-flight/in-flight.hxx>

#include <drogon/drogon.h>
#include <exception>
#include <trantor/utils/Logger.h>
#include <utility>

namespace
{
constexpr double kRefreshSeconds = 30.0;
constexpr double kFollowSweepSeconds = 1.0;
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

void LookRefresh::start()
{
  if (running_.exchange(true))
    return;
  drogon::app().getLoop()->runEvery(kFollowSweepSeconds, [this] {
    if (running_.load())
      FollowService::sweep();
  });
  drogon::app().getLoop()->runEvery(kRefreshSeconds, [this] {
    if (!running_.load())
      return;
    drogon::async_run([this]() -> drogon::Task<void> {
      const in_flight::Guard guard(inFlight_);
      try {
        co_await tick();
      }
      catch (const std::exception& error) {
        LOG_WARN << "Camera look: refresh failed: " << error.what();
      }
      co_return;
    });
  });
}

void LookRefresh::requestStop()
{
  running_.store(false);
}

bool LookRefresh::drained() const
{
  return inFlight_.load() == 0;
}

drogon::Task<void> LookRefresh::tick()
{
  std::unordered_map<int64_t, AimSettings> settings;
  for (const CameraSchema& camera : co_await repository_.findEnabled())
    settings[camera.id] = aim_settings::parse({.capabilities = camera.capabilities, .config = camera.config});
  LookableCameras::instance().replace(std::move(settings));
}
