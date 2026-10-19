#include "watch-aim.hxx"

#include <feature/camera-control/services/aim-board.hxx>
#include <feature/camera-control/services/ptz-lease.hxx>
#include <feature/watch/services/watch-gate.hxx>

#include <shared/services/camera-driver/camera-scene-log.hxx>
#include <trantor/utils/Logger.h>

#include <algorithm>
#include <cmath>
#include <thread>
#include <utility>

namespace
{

namespace v1 = argus::camera::v1;

constexpr double kFaceLift = 1.0 / 3.0;
constexpr int64_t kLookPollMs = 200;

int64_t epochMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

v1::AimResponse answer(v1::AimOutcome outcome, std::string detail)
{
  v1::AimResponse response;
  response.set_outcome(outcome);
  response.set_detail(std::move(detail));
  return response;
}

bool argusHolds(int64_t cameraId)
{
  return PtzLease::instance().holds({.cameraId = cameraId, .holder = PtzHolder::Argus, .userId = 0, .atMs = epochMs()});
}

void appendIds(const Json::Value& ids, std::vector<std::string>& out)
{
  if (!ids.isArray())
    return;
  for (const Json::Value& id : ids) {
    if (id.isString() && !id.asString().empty())
      out.push_back(id.asString());
    else if (id.isIntegral())
      out.push_back(std::to_string(id.asInt64()));
  }
}

}

AimStep aimStepFor(const PersonSighting& sighting, const AimGeometry& geometry)
{
  const double faceY = static_cast<double>(sighting.centerY) - (static_cast<double>(sighting.height) * kFaceLift / 2.0);
  const double dx = static_cast<double>(sighting.centerX) - 0.5;
  const double dy = std::clamp(faceY, 0.0, 1.0) - 0.5;
  const bool centered = std::abs(dx) <= geometry.deadZone && std::abs(dy) <= geometry.deadZone;
  if (centered)
    return {.pan = 0, .tilt = 0, .centered = true};
  const auto pan = std::abs(dx) <= geometry.deadZone
                       ? int64_t{0}
                       : static_cast<int64_t>(std::lround(dx * geometry.horizontalFovDegrees));
  const auto tilt = std::abs(dy) <= geometry.deadZone
                        ? int64_t{0}
                        : static_cast<int64_t>(std::lround(-dy * geometry.verticalFovDegrees));
  return {.pan = pan, .tilt = tilt, .centered = pan == 0 && tilt == 0};
}

std::optional<PersonSighting> bestSighting(const SightingChoice& choice)
{
  std::optional<PersonSighting> best;
  for (const PersonSighting& sighting : choice.sightings) {
    if (sighting.atMs < choice.notBeforeMs)
      continue;
    if (!best || sighting.atMs > best->atMs ||
        (sighting.atMs == best->atMs && sighting.width * sighting.height > best->width * best->height))
      best = sighting;
  }
  return best;
}

std::vector<std::string> presetIdsOf(const Json::Value& data)
{
  std::vector<std::string> out;
  if (!data.isObject())
    return out;
  const Json::Value& preset = data["preset"];
  if (!preset.isObject())
    return out;
  if (preset["id"].isArray())
    appendIds(preset["id"], out);
  else if (preset["preset"].isObject())
    appendIds(preset["preset"]["id"], out);
  return out;
}

AimGeometry aimGeometryOf(const CameraWatchConfig& config)
{
  return {.horizontalFovDegrees = static_cast<double>(config.aimHorizontalFovDegrees),
          .verticalFovDegrees = static_cast<double>(config.aimVerticalFovDegrees),
          .deadZone = static_cast<double>(config.aimDeadZonePercent) / 100.0};
}

WatchAimer::WatchAimer(WatchAimerInput input) : input_(std::move(input)), geometry_(aimGeometryOf(input_.config)) {}

v1::AimResponse WatchAimer::find(const AimTarget& target)
{
  const int64_t cameraId = target.cameraId;
  const v1::WatchRefusal refusal = input_.gate ? input_.gate(cameraId) : v1::WATCH_REFUSAL_UNKNOWN_CAMERA;
  if (refusal != v1::WATCH_REFUSAL_NONE)
    return answer(v1::AIM_OUTCOME_REFUSED, std::string(watchRefusalName(refusal)));
  const std::optional<WatchCameraFlags> flags = input_.board->flags(cameraId);
  if (!flags || !flags->panTilt)
    return answer(v1::AIM_OUTCOME_NO_PTZ, {});
  const std::optional<CameraSchema> schema = input_.board->schema(cameraId);
  const std::shared_ptr<ICameraDriver> driver = schema ? CameraDriverRegistry::instance().driverFor(*schema) : nullptr;
  if (!driver)
    return answer(v1::AIM_OUTCOME_NO_PTZ, {});
  const int64_t now = epochMs();
  if (PtzLease::instance().claim({.cameraId = cameraId, .holder = PtzHolder::Argus, .userId = 0, .atMs = now}) ==
      PtzClaimResult::Refused)
    return answer(v1::AIM_OUTCOME_REFUSED, "lease_held");
  hold(target);
  const CameraScene scene = CameraSceneLog::instance().sceneOf(cameraId);
  const int64_t notBefore =
      std::max({now - input_.config.aimInFrameMs, scene.aimedAtMs, scene.moveEndedAtMs});
  if (const std::optional<PersonSighting> seen = lookFor(cameraId, notBefore)) {
    if (aimStepFor(*seen, geometry_).centered)
      return answer(v1::AIM_OUTCOME_IN_FRAME, {});
    return center(*driver, cameraId, *seen) ? answer(v1::AIM_OUTCOME_CENTERED, {})
                                            : answer(v1::AIM_OUTCOME_IN_FRAME, "move_failed");
  }
  const DriverResult listed = driver->presets();
  std::vector<std::string> presets = listed.ok ? presetIdsOf(listed.data) : std::vector<std::string>{};
  if (std::cmp_greater(presets.size(), input_.config.aimMaxPresets))
    presets.resize(static_cast<size_t>(input_.config.aimMaxPresets));
  for (const std::string& presetId : presets) {
    if (input_.gate(cameraId) != v1::WATCH_REFUSAL_NONE || !argusHolds(cameraId))
      return answer(v1::AIM_OUTCOME_REFUSED, "interrupted");
    if (!goPreset(*driver, cameraId, presetId))
      continue;
    const int64_t lookFrom = epochMs();
    const int64_t lookUntil = lookFrom + input_.config.aimLookMs;
    while (epochMs() < lookUntil) {
      if (const std::optional<PersonSighting> seen = lookFor(cameraId, lookFrom)) {
        if (aimStepFor(*seen, geometry_).centered || !center(*driver, cameraId, *seen))
          return answer(v1::AIM_OUTCOME_IN_FRAME, presetId);
        return answer(v1::AIM_OUTCOME_CENTERED, presetId);
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(kLookPollMs));
    }
  }
  return answer(v1::AIM_OUTCOME_NOT_FOUND, {});
}

v1::AimResponse WatchAimer::release(const AimTarget& target)
{
  uint64_t generation = 0;
  {
    std::scoped_lock lock(mutex_);
    const auto found = holding_.find(target.cameraId);
    if (found == holding_.end() || found->second.conversationId != target.conversationId)
      return answer(v1::AIM_OUTCOME_RELEASED, "not_held");
    generation = found->second.generation;
  }
  const int64_t cameraId = target.cameraId;
  if (input_.schedule)
    input_.schedule(std::chrono::milliseconds(input_.config.aimReturnMs),
                    [this, cameraId, generation] { returnWhenIdle(cameraId, generation); });
  else
    returnWhenIdle(cameraId, generation);
  return answer(v1::AIM_OUTCOME_RELEASED, {});
}

bool WatchAimer::holds(int64_t cameraId)
{
  std::scoped_lock lock(mutex_);
  return holding_.contains(cameraId);
}

void WatchAimer::markAway(int64_t cameraId)
{
  AimBoard::instance().setActivity({.cameraId = cameraId, .activity = AimActivity::Looking});
  AimBoard::instance().lose(cameraId);
  CameraSceneLog::instance().setArgusAway({.cameraId = cameraId, .away = true});
}

std::optional<PersonSighting> WatchAimer::lookFor(int64_t cameraId, int64_t notBeforeMs) const
{
  const std::vector<PersonSighting> recent =
      PersonSightings::instance().recent({.cameraId = cameraId, .sinceMs = notBeforeMs});
  return bestSighting({.sightings = recent, .notBeforeMs = notBeforeMs});
}

bool WatchAimer::center(ICameraDriver& driver, int64_t cameraId, const PersonSighting& sighting) const
{
  const AimStep step = aimStepFor(sighting, geometry_);
  if (step.centered || !argusHolds(cameraId))
    return false;
  const int64_t started = epochMs();
  markAway(cameraId);
  CameraSceneLog::instance().beginMove(
      {.cameraId = cameraId, .startMs = started, .expectedEndMs = started + input_.config.aimSettleMs, .returnsHome = false});
  const DriverResult moved = driver.move({.x = step.pan, .y = step.tilt, .angle = std::nullopt, .stop = false});
  std::this_thread::sleep_for(std::chrono::milliseconds(input_.config.aimSettleMs));
  const int64_t ended = epochMs();
  CameraSceneLog::instance().endMove({.cameraId = cameraId, .atMs = ended});
  CameraSceneLog::instance().noteAimed({.cameraId = cameraId, .atMs = ended});
  if (!moved.ok)
    LOG_WARN << "Camera watch: aiming camera " << cameraId << " failed: " << moved.error;
  return moved.ok;
}

bool WatchAimer::goPreset(ICameraDriver& driver, int64_t cameraId, const std::string& presetId) const
{
  const int64_t started = epochMs();
  markAway(cameraId);
  CameraSceneLog::instance().beginMove(
      {.cameraId = cameraId, .startMs = started, .expectedEndMs = started + input_.config.aimSettleMs, .returnsHome = false});
  const DriverResult moved = driver.preset({.action = "goto", .id = presetId, .name = {}});
  std::this_thread::sleep_for(std::chrono::milliseconds(input_.config.aimSettleMs));
  const int64_t ended = epochMs();
  CameraSceneLog::instance().endMove({.cameraId = cameraId, .atMs = ended});
  CameraSceneLog::instance().noteAimed({.cameraId = cameraId, .atMs = ended});
  return moved.ok;
}

void WatchAimer::hold(const AimTarget& target)
{
  std::scoped_lock lock(mutex_);
  holding_[target.cameraId] = {.conversationId = target.conversationId, .generation = nextGeneration_++};
}

void WatchAimer::returnWhenIdle(int64_t cameraId, uint64_t generation)
{
  {
    std::scoped_lock lock(mutex_);
    const auto found = holding_.find(cameraId);
    if (found == holding_.end() || found->second.generation != generation)
      return;
    holding_.erase(found);
  }
  if (!argusHolds(cameraId))
    return;
  drogon::async_run([this, cameraId]() -> drogon::Task<void> { co_await returnHome(cameraId); });
}

drogon::Task<void> WatchAimer::returnHome(int64_t cameraId) const
{
  const CameraControlResult result = co_await control_.returnHome({.cameraId = cameraId, .reason = "conversation"});
  if (!result || !result->ok)
    PtzLease::instance().release({.cameraId = cameraId, .holder = PtzHolder::Argus, .userId = 0, .atMs = epochMs()});
}
