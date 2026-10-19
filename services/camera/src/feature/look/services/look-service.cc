#include "look-service.hxx"

#include <feature/camera-control/services/aim-board.hxx>
#include <feature/camera-control/services/aim-settings.hxx>
#include <feature/look/services/follow-service.hxx>
#include <feature/look/services/second-look.hxx>
#include <feature/recording/services/clip-recorder.hxx>
#include <shared/services/privacy-state/privacy-state.hxx>

#include <algorithm>
#include <chrono>
#include <drogon/drogon.h>
#include <trantor/utils/Logger.h>

namespace
{
int64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
}

namespace look_plan
{
LookPlan plan(const LookPlanInput& input)
{
  if (!input.lookable)
    return {.move = false, .delta = {}, .refusal = "not_lookable"};
  if (!input.positionKnown)
    return {.move = false, .delta = {}, .refusal = "position_unknown"};
  const auto offset = aim_math::centerOffset(
      {.box = input.box,
       .fov = {.horizontalDeg = input.ptz.fovHorizontalDeg, .verticalDeg = input.ptz.fovVerticalDeg},
       .deadZone = input.ptz.deadZone});
  if (!offset)
    return {.move = false, .delta = {}, .refusal = "centered"};
  if (aim_math::entersSector({.from = input.position, .delta = *offset, .sectors = input.sectors}))
    return {.move = false, .delta = {}, .refusal = "no_aim_sector"};
  return {.move = true, .delta = *offset, .refusal = {}};
}

NormalizedBox centered(const NormalizedBox& box)
{
  return {.x = std::clamp(0.5 - box.w / 2.0, 0.0, 1.0),
          .y = std::clamp(0.5 - box.h / 2.0, 0.0, 1.0),
          .w = box.w,
          .h = box.h};
}
}

drogon::Task<LookOutcome> LookService::look(LookRequest request) const
{
  LookOutcome outcome;
  const int64_t started = nowMs();
  const auto camera = co_await repository_.findById(request.cameraId);
  if (!camera) {
    outcome.detail = "camera_not_found";
    co_return outcome;
  }
  if (PrivacyStateService::instance().masked(request.cameraId)) {
    outcome.detail = "privacy_on";
    co_return outcome;
  }
  const SecondLook* second = SecondLook::installed();
  if (second == nullptr) {
    outcome.detail = "no_detector";
    co_return outcome;
  }
  const CameraPtzConfig& ptz = CameraControlFeatureService::ptz();
  const int64_t budgetMs = request.budgetMs > 0 ? std::min(request.budgetMs, ptz.lookBudgetMs) : ptz.lookBudgetMs;
  const AimSettings settings = aim_settings::parse({.capabilities = camera->capabilities, .config = camera->config});
  AimSnapshot aim = AimBoard::instance().snapshot(request.cameraId);
  NormalizedBox box = request.box;
  const LookPlan plan = look_plan::plan({.box = box,
                                         .position = aim.position,
                                         .positionKnown = aim.known,
                                         .lookable = settings.lookable(),
                                         .sectors = settings.noAimSectors,
                                         .ptz = ptz});
  if (plan.move && AimBoard::instance().wearAllows({.cameraId = request.cameraId,
                                                     .nowMs = started,
                                                     .perMinute = ptz.movesPerMinute,
                                                     .dailyCap = ptz.dailyMoveCap})) {
    const auto moved = co_await control_.aimByArgus(
        {.cameraId = request.cameraId, .delta = plan.delta, .reason = request.reason, .activity = AimActivity::Looking});
    if (moved && moved->ok) {
      outcome.moved = true;
      co_await drogon::sleepCoro(drogon::app().getLoop(), std::chrono::milliseconds(ptz.settleMs));
      box = look_plan::centered(box);
      aim = AimBoard::instance().snapshot(request.cameraId);
    }
  }
  else if (!plan.refusal.empty()) {
    outcome.detail = plan.refusal;
  }
  const int64_t remaining = std::max<int64_t>(budgetMs - (nowMs() - started), 500);
  const SecondLookResult result = co_await second->confirm(
      {.cameraId = request.cameraId,
       .cameraName = camera->name,
       .box = box,
       .grabs = second->config().grabs,
       .budgetMs = remaining,
       .sectors = settings.noAimSectors,
       .position = aim.position,
       .awayFromHome = !aim.atHome,
       .fov = {.horizontalDeg = ptz.fovHorizontalDeg, .verticalDeg = ptz.fovVerticalDeg}});
  outcome.confirmed = result.confirmed;
  outcome.cls = result.cls;
  outcome.confidence = result.confidence;
  for (const std::string& crop : result.crops) {
    const int64_t id = co_await ClipRecorder::instance().keepStill(
        {.cameraId = request.cameraId, .trigger = CameraClipTrigger::Look, .jpeg = crop, .atEpochMs = 0});
    if (id > 0)
      outcome.cropIds.push_back(id);
  }
  const bool following = outcome.confirmed && request.follow && settings.followIntruders && settings.lookable();
  if (following)
    FollowBoard::instance().start({.cameraId = request.cameraId, .trackId = request.trackId, .atMs = nowMs()});
  else if (outcome.moved && drogon::app().isRunning()) {
    const int64_t cameraId = request.cameraId;
    drogon::app().getLoop()->runAfter(static_cast<double>(ptz.returnAfterLookMs) / 1000.0, [cameraId] {
      if (FollowBoard::instance().following(cameraId))
        return;
      drogon::async_run([cameraId]() -> drogon::Task<void> {
        const CameraControlFeatureService control;
        static_cast<void>(co_await control.returnHome({.cameraId = cameraId, .reason = "look"}));
      });
    });
  }
  LOG_INFO << "Camera look: camera " << request.cameraId << " " << (outcome.confirmed ? "confirmed" : "discarded")
           << " the " << request.reason << " in " << (nowMs() - started) << " ms"
           << (outcome.moved ? " after a move" : "") << (following ? "; following the target" : "");
  co_return outcome;
}
