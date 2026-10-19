#include "camera-control-feature-service.hxx"

#include <camera/camera-errors.hxx>
#include <drogon/utils/Utilities.h>
#include <errors/response-exception.hxx>
#include <feature/camera-control/services/duty-gate.hxx>
#include <feature/camera-control/services/aim-board.hxx>
#include <feature/camera-control/services/aim-settings.hxx>
#include <cmath>
#include <runtime/blocking-task.hxx>
#include <shared/repositories/camera/camera-repository.hxx>
#include <shared/services/camera-driver/camera-scene-log.hxx>
#include <shared/services/privacy-state/privacy-state.hxx>
#include <sync/camera-change-sink.hxx>
#include <shared/services/camera-driver/stream-only-driver.hxx>
#include <shared/services/privacy/privacy-mask.hxx>
#include <shared/services/stream/snapshot-store.hxx>

#include <text/json-util.hxx>

#include <algorithm>
#include <tuple>
#include <string_view>
#include <drogon/HttpAppFramework.h>
#include <chrono>
#include <utility>

namespace
{
constexpr int64_t kFreshSnapshotMs = 10000;
constexpr std::string_view kHomePresetKey = "homePreset";

CameraPtzConfig& ptzConfig()
{
  static CameraPtzConfig config;
  return config;
}

std::string homePresetOf(const std::string& config)
{
  const Json::Value parsed = json_util::fromString(config);
  if (!parsed.isObject())
    return {};
  const Json::Value& home = parsed[std::string(kHomePresetKey)];
  return home.isString() ? home.asString() : std::string();
}

int64_t nowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}

bool succeeded(const CameraControlResult& result)
{
  return result && result->ok;
}

DriverResult stillOf(const CameraSnapshot& picture)
{
  Json::Value out;
  out["image"] = "data:image/jpeg;base64," + drogon::utils::base64Encode(picture.jpeg);
  out["capturedAt"] = static_cast<Json::Int64>(picture.atMs);
  return {.ok = true, .error = {}, .data = out};
}
}

drogon::Task<CameraControlResult> CameraControlFeatureService::onDevice(
    int64_t cameraId,
    const std::function<DriverResult(ICameraDriver&)>& work) const
{
  const auto camera = co_await cameraRepository_.findById(cameraId);
  if (!camera)
    co_return std::nullopt;

  std::shared_ptr<ICameraDriver> driver =
      CameraDriverRegistry::instance().controlDriverFor(*camera);
  if (!driver)
    driver = std::make_shared<StreamOnlyDriver>(*camera);

  DriverResult result =
      co_await BlockingTask<DriverResult>([driver, work]() { return work(*driver); });
  result.attempted = true;
  co_return result;
}

drogon::Task<CameraControlResult>
CameraControlFeatureService::status(int64_t cameraId) const
{
  auto result = co_await onDevice(
      cameraId, [](ICameraDriver& driver) { return driver.status(); });
  if (result && result->ok)
  {
    CameraRepository repository;
    const auto camera = co_await repository.findById(cameraId);
    if (camera)
      co_await PrivacyStateService::instance().refresh(
          {.camera = *camera, .driverStatus = result->data, .readable = true});
  }
  co_return result;
}

drogon::Task<CameraControlResult>
CameraControlFeatureService::capabilities(int64_t cameraId) const
{
  co_return co_await onDevice(cameraId, [](ICameraDriver& driver) {
    return DriverResult{.ok = true, .error = {}, .data = driver.discoverCapabilities()};
  });
}

drogon::Task<CameraControlResult>
CameraControlFeatureService::presets(int64_t cameraId) const
{
  co_return co_await onDevice(cameraId,
                              [](ICameraDriver& driver) { return driver.presets(); });
}

void CameraControlFeatureService::configurePtz(const CameraPtzConfig& config)
{
  ptzConfig() = config;
  PtzLease::instance().configure({.parkMs = config.parkMs, .idleReleaseMs = config.idleReleaseMs});
}

drogon::Task<CameraControlResult>
CameraControlFeatureService::move(const CameraMoveInput& input) const
{
  const int64_t cameraId = input.cameraId;
  const CameraPtzDto& body = input.body;
  co_await admitActor({.userId = input.actorUserId, .role = input.actorRole, .changesPresets = false});
  if (PrivacyStateService::instance().masked(cameraId))
    co_return DriverResult{.ok = false,
                            .error = "This camera is in privacy mode",
                            .data = Json::Value(),
                            .attempted = true,
                            .failure = "privacy_on"};
  std::ignore = PtzLease::instance().claim(
      {.cameraId = cameraId, .holder = PtzHolder::Person, .userId = input.actorUserId, .atMs = nowMs()});
  auto result = co_await onDevice(cameraId, [body](ICameraDriver& driver) {
    return driver.move({.x = body.x, .y = body.y, .angle = body.angle, .stop = body.stop});
  });
  if (succeeded(result)) {
    CameraSceneLog::instance().noteAimed({.cameraId = cameraId, .atMs = nowMs()});
    if (!body.stop && !body.angle && (body.x || body.y))
      AimBoard::instance().recordMove(
          {.cameraId = cameraId,
           .delta = {.pan = static_cast<double>(body.x.value_or(0)), .tilt = static_cast<double>(body.y.value_or(0))},
           .atMs = nowMs()});
    else if (body.angle)
      AimBoard::instance().lose(cameraId);
  }
  co_await publishAim(cameraId);
  co_return result;
}

drogon::Task<void>
CameraControlFeatureService::announceStatus(int64_t cameraId) const
{
  const CameraChangeSink* sink = camera_change::getSink();
  if (sink == nullptr)
    co_return;
  co_await sink->publishResource({.scope = ResourceScope::Status,
                                  .ids = {cameraId},
                                  .user = 0,
                                  .client = nullptr});
}

drogon::Task<void>
CameraControlFeatureService::announcePrivacy(int64_t cameraId) const
{
  const CameraChangeSink* sink = camera_change::getSink();
  if (sink == nullptr)
    co_return;
  co_await sink->publishResource({.scope = ResourceScope::Status,
                                  .ids = {cameraId},
                                  .user = 0,
                                  .client = nullptr});
  co_await sink->publishResource({.scope = ResourceScope::Overview,
                                  .ids = {},
                                  .user = 0,
                                  .client = nullptr});
}

drogon::Task<void> CameraControlFeatureService::recordPrivacyAction(
    const CameraSettingsInput& input, CameraPrivacyState before,
    CameraPrivacyState after) const
{
  const CameraChangeSink* sink = camera_change::getSink();
  if (sink == nullptr)
    co_return;
  Json::Value oldData(Json::objectValue);
  oldData["privacyState"] = cameraPrivacyStateToString(before);
  Json::Value newData(Json::objectValue);
  newData["privacyState"] = cameraPrivacyStateToString(after);
  newData["requested"] = input.body.privacy.value_or(false);
  co_await sink->publishAction(
      {.event = {.userId = input.actorUserId,
                 .recordId = input.cameraId,
                 .tableName = TableName::Camera,
                 .action = UserAction::Update,
                 .oldData = std::move(oldData),
                 .newData = std::move(newData),
                 .ipAddress = input.actorIp},
       .client = nullptr});
}

drogon::Task<CameraControlResult>
CameraControlFeatureService::preset(const CameraPresetInput& input) const
{
  const int64_t cameraId = input.cameraId;
  const CameraPresetDto& body = input.body;
  co_await admitActor({.userId = input.actorUserId,
                       .role = input.actorRole,
                       .changesPresets = body.action == "save" || body.action == "delete" ||
                                         body.action == "set_home"});
  if (body.action == "set_home")
    co_return co_await setHome(input);
  if (body.action == "go_home")
    co_return co_await returnHome({.cameraId = cameraId, .reason = "person"});
  if (PrivacyStateService::instance().masked(cameraId))
    co_return DriverResult{.ok = false,
                            .error = "This camera is in privacy mode",
                            .data = Json::Value(),
                            .attempted = true,
                            .failure = "privacy_on"};
  if (body.action == "goto")
    std::ignore = PtzLease::instance().claim(
        {.cameraId = cameraId, .holder = PtzHolder::Person, .userId = input.actorUserId, .atMs = nowMs()});
  auto result = co_await onDevice(cameraId, [body](ICameraDriver& driver) {
    return driver.preset({.action = body.action, .id = body.id, .name = body.name});
  });
  if (succeeded(result) && body.action != "save" && body.action != "delete")
    CameraSceneLog::instance().noteAimed({.cameraId = cameraId, .atMs = nowMs()});
  if (succeeded(result) && body.action == "goto") {
    const auto camera = co_await cameraRepository_.findById(cameraId);
    if (camera && homePresetOf(camera->config) == body.id)
      AimBoard::instance().arriveHome(cameraId);
    else
      AimBoard::instance().lose(cameraId);
    co_await publishAim(cameraId);
  }
  if (succeeded(result) && (body.action == "save" || body.action == "delete"))
    co_await announceStatus(cameraId);
  co_return result;
}

drogon::Task<void> CameraControlFeatureService::admitActor(const ActorCheck& actor)
{
  if (actor.role != UserRole::Guard)
    co_return;
  if (actor.changesPresets)
    throw ResponseException(CameraErrors::PresetsNotYours);
  if (!co_await DutyGate::instance().onDuty(actor.userId))
    throw ResponseException(CameraErrors::GuardOffDuty);
}

drogon::Task<CameraControlResult>
CameraControlFeatureService::setHome(const CameraPresetInput& input) const
{
  const auto camera = co_await cameraRepository_.findById(input.cameraId);
  if (!camera)
    co_return std::nullopt;
  Json::Value config = json_util::fromString(camera->config);
  if (!config.isObject())
    config = Json::Value(Json::objectValue);
  config[std::string(kHomePresetKey)] = input.body.id;
  const Json::Value before = camera->toJson();
  CameraUpdateInput change;
  change.config = json_util::toString(config);
  const auto after = co_await cameraRepository_.update(input.cameraId, change);
  if (const CameraChangeSink* sink = camera_change::getSink())
    co_await sink->publishAudit({.recordId = input.cameraId,
                                 .tableName = TableName::Camera,
                                 .before = before,
                                 .after = after.toJson(),
                                 .actorId = input.actorUserId,
                                 .client = nullptr});
  Json::Value data(Json::objectValue);
  data["homePreset"] = input.body.id;
  co_await publishAim(input.cameraId);
  co_return DriverResult{.ok = true, .error = {}, .data = data};
}

drogon::Task<void> CameraControlFeatureService::recordAutomaticMove(const AutomaticMove& move) const
{
  const CameraChangeSink* sink = camera_change::getSink();
  if (sink == nullptr)
    co_return;
  Json::Value newData(Json::objectValue);
  newData["event"] = "ptz_automatic_move";
  newData["reason"] = move.reason;
  newData["target"] = move.target;
  newData["durationMs"] = static_cast<Json::Int64>(move.durationMs);
  co_await sink->publishAction({.event = {.userId = 0,
                                          .recordId = move.cameraId,
                                          .tableName = TableName::Camera,
                                          .action = UserAction::Update,
                                          .oldData = Json::Value(),
                                          .newData = std::move(newData),
                                          .ipAddress = ""},
                                .client = nullptr});
}

drogon::Task<CameraControlResult>
CameraControlFeatureService::returnHome(const CameraReturnHomeInput& input) const
{
  const int64_t cameraId = input.cameraId;
  const auto camera = co_await cameraRepository_.findById(cameraId);
  if (!camera)
    co_return std::nullopt;
  const std::string home = homePresetOf(camera->config);
  if (home.empty())
    co_return DriverResult{.ok = false,
                           .error = "This camera has no home position",
                           .data = Json::Value(),
                           .attempted = false,
                           .failure = "no_home"};
  if (PrivacyStateService::instance().masked(cameraId))
    co_return DriverResult{.ok = false,
                           .error = "This camera is in privacy mode",
                           .data = Json::Value(),
                           .attempted = true,
                           .failure = "privacy_on"};
  const bool byArgus = input.reason != "person";
  const int64_t started = nowMs();
  if (byArgus) {
    const PtzClaimResult claim =
        PtzLease::instance().claim({.cameraId = cameraId, .holder = PtzHolder::Argus, .userId = 0, .atMs = started});
    if (claim == PtzClaimResult::Refused)
      co_return DriverResult{.ok = false,
                             .error = "Someone is moving this camera",
                             .data = Json::Value(),
                             .attempted = false,
                             .failure = "lease_held"};
  }
  const int64_t settle = ptzConfig().settleMs;
  if (byArgus)
    AimBoard::instance().setActivity({.cameraId = cameraId, .activity = AimActivity::Returning});
  CameraSceneLog::instance().setArgusAway({.cameraId = cameraId, .away = false});
  CameraSceneLog::instance().beginMove(
      {.cameraId = cameraId, .startMs = started, .expectedEndMs = started + settle, .returnsHome = true});
  const auto result = co_await onDevice(cameraId, [home, byArgus](ICameraDriver& driver) {
    std::optional<bool> tracked;
    if (byArgus) {
      const DriverResult status = driver.status();
      if (status.ok && status.data["autoTrackEnabled"].isBool())
        tracked = status.data["autoTrackEnabled"].asBool();
      if (tracked.value_or(false))
        std::ignore = driver.settings({.privacy = std::nullopt,
                                       .led = std::nullopt,
                                       .dayNight = std::nullopt,
                                       .motion = std::nullopt,
                                       .motionSensitivity = std::nullopt,
                                       .autoTrack = false,
                                       .alarm = std::nullopt,
                                       .alarmVolume = std::nullopt,
                                       .sounding = std::nullopt});
    }
    DriverResult moved = driver.preset({.action = "goto", .id = home, .name = {}});
    if (tracked.value_or(false))
      std::ignore = driver.settings({.privacy = std::nullopt,
                                     .led = std::nullopt,
                                     .dayNight = std::nullopt,
                                     .motion = std::nullopt,
                                     .motionSensitivity = std::nullopt,
                                     .autoTrack = true,
                                     .alarm = std::nullopt,
                                     .alarmVolume = std::nullopt,
                                     .sounding = std::nullopt});
    return moved;
  });
  drogon::app().getLoop()->runAfter(static_cast<double>(settle) / 1000.0, [cameraId, byArgus] {
    const int64_t ended = nowMs();
    CameraSceneLog::instance().endMove({.cameraId = cameraId, .atMs = ended});
    if (byArgus)
      PtzLease::instance().release({.cameraId = cameraId, .holder = PtzHolder::Argus, .userId = 0, .atMs = ended});
  });
  if (succeeded(result))
    AimBoard::instance().arriveHome(cameraId);
  else
    AimBoard::instance().setActivity({.cameraId = cameraId, .activity = AimActivity::Idle});
  if (byArgus && succeeded(result))
    co_await recordAutomaticMove(
        {.cameraId = cameraId, .reason = input.reason, .target = "home", .durationMs = nowMs() - started});
  co_await publishAim(cameraId);
  co_return result;
}

const CameraPtzConfig& CameraControlFeatureService::ptz()
{
  return ptzConfig();
}

drogon::Task<void> CameraControlFeatureService::publishAim(int64_t cameraId) const
{
  const CameraChangeSink* sink = camera_change::getSink();
  if (sink == nullptr)
    co_return;
  co_await sink->publishResource({.scope = ResourceScope::Aim, .ids = {cameraId}, .user = 0, .client = nullptr});
}

drogon::Task<CameraControlResult> CameraControlFeatureService::aimByArgus(const CameraAimInput& input) const
{
  const int64_t cameraId = input.cameraId;
  if (PrivacyStateService::instance().masked(cameraId))
    co_return DriverResult{.ok = false,
                           .error = "This camera is in privacy mode",
                           .data = Json::Value(),
                           .attempted = false,
                           .failure = "privacy_on"};
  const int64_t started = nowMs();
  const PtzClaimResult claim =
      PtzLease::instance().claim({.cameraId = cameraId, .holder = PtzHolder::Argus, .userId = 0, .atMs = started});
  if (claim == PtzClaimResult::Refused)
    co_return DriverResult{.ok = false,
                           .error = "Someone is moving this camera",
                           .data = Json::Value(),
                           .attempted = false,
                           .failure = "lease_held"};
  const int64_t settle = ptzConfig().settleMs;
  const auto pan = static_cast<int64_t>(std::lround(input.delta.pan));
  const auto tilt = static_cast<int64_t>(std::lround(input.delta.tilt));
  AimBoard::instance().setActivity({.cameraId = cameraId, .activity = input.activity});
  CameraSceneLog::instance().setArgusAway({.cameraId = cameraId, .away = true});
  CameraSceneLog::instance().beginMove(
      {.cameraId = cameraId, .startMs = started, .expectedEndMs = started + settle, .returnsHome = true});
  const auto result = co_await onDevice(cameraId, [pan, tilt](ICameraDriver& driver) {
    return driver.move({.x = pan, .y = tilt, .angle = std::nullopt, .stop = false});
  });
  if (succeeded(result))
    AimBoard::instance().recordMove(
        {.cameraId = cameraId,
         .delta = {.pan = static_cast<double>(pan), .tilt = static_cast<double>(tilt)},
         .atMs = started});
  if (drogon::app().isRunning())
    drogon::app().getLoop()->runAfter(static_cast<double>(settle) / 1000.0, [cameraId] {
      CameraSceneLog::instance().endMove({.cameraId = cameraId, .atMs = nowMs()});
    });
  else
    CameraSceneLog::instance().endMove({.cameraId = cameraId, .atMs = started + settle});
  if (succeeded(result))
    co_await recordAutomaticMove({.cameraId = cameraId,
                                  .reason = input.reason,
                                  .target = "pan " + std::to_string(pan) + " tilt " + std::to_string(tilt),
                                  .durationMs = settle});
  co_await publishAim(cameraId);
  co_return result;
}

drogon::Task<CameraControlResult> CameraControlFeatureService::gotoByArgus(const CameraArgusGotoInput& input) const
{
  const int64_t cameraId = input.cameraId;
  if (PrivacyStateService::instance().masked(cameraId))
    co_return DriverResult{.ok = false,
                           .error = "This camera is in privacy mode",
                           .data = Json::Value(),
                           .attempted = false,
                           .failure = "privacy_on"};
  const int64_t started = nowMs();
  if (PtzLease::instance().claim({.cameraId = cameraId, .holder = PtzHolder::Argus, .userId = 0, .atMs = started}) ==
      PtzClaimResult::Refused)
    co_return DriverResult{.ok = false,
                           .error = "Someone is moving this camera",
                           .data = Json::Value(),
                           .attempted = false,
                           .failure = "lease_held"};
  const int64_t settle = ptzConfig().settleMs;
  AimBoard::instance().setActivity({.cameraId = cameraId, .activity = AimActivity::Patrolling});
  CameraSceneLog::instance().setArgusAway({.cameraId = cameraId, .away = true});
  CameraSceneLog::instance().beginMove(
      {.cameraId = cameraId, .startMs = started, .expectedEndMs = started + settle, .returnsHome = true});
  const std::string preset = input.presetId;
  const auto result = co_await onDevice(cameraId, [preset](ICameraDriver& driver) {
    return driver.preset({.action = "goto", .id = preset, .name = {}});
  });
  if (succeeded(result))
    AimBoard::instance().recordMove({.cameraId = cameraId, .delta = {}, .atMs = started});
  AimBoard::instance().lose(cameraId);
  if (drogon::app().isRunning())
    drogon::app().getLoop()->runAfter(static_cast<double>(settle) / 1000.0, [cameraId] {
      CameraSceneLog::instance().endMove({.cameraId = cameraId, .atMs = nowMs()});
    });
  else
    CameraSceneLog::instance().endMove({.cameraId = cameraId, .atMs = started + settle});
  if (succeeded(result))
    co_await recordAutomaticMove(
        {.cameraId = cameraId, .reason = input.reason, .target = "preset " + preset, .durationMs = settle});
  co_await publishAim(cameraId);
  co_return result;
}

drogon::Task<Json::Value> CameraControlFeatureService::aimState(const CameraAimStateInput& input) const
{
  const auto camera = co_await cameraRepository_.findById(input.cameraId);
  if (!camera)
    throw ResponseException(CameraErrors::CameraNotFound);
  const int64_t now = nowMs();
  const AimSettings settings = aim_settings::parse({.capabilities = camera->capabilities, .config = camera->config});
  const PtzLeaseState lease = PtzLease::instance().stateOf({.cameraId = input.cameraId, .atMs = now});
  const AimSnapshot aim = AimBoard::instance().snapshot(input.cameraId);
  Json::Value json(Json::objectValue);
  json["cameraId"] = static_cast<Json::Int64>(input.cameraId);
  json["ptz"] = settings.ptz;
  json["homePreset"] = settings.homePreset.empty() ? Json::Value() : Json::Value(settings.homePreset);
  json["atHome"] = aim.atHome;
  json["positionKnown"] = aim.known;
  json["pan"] = aim.position.pan;
  json["tilt"] = aim.position.tilt;
  Json::Value holder(Json::objectValue);
  holder["holder"] = ptzHolderToString(lease.holder);
  holder["userId"] = static_cast<Json::Int64>(lease.userId);
  holder["mine"] = lease.holder != PtzHolder::None && lease.holder != PtzHolder::Argus &&
                   lease.userId == input.userId;
  holder["sinceMs"] = static_cast<Json::Int64>(lease.sinceMs);
  holder["parkUntilMs"] = static_cast<Json::Int64>(lease.parkUntilMs);
  json["lease"] = holder;
  json["activity"] = aimActivityToString(aim.activity);
  json["settings"] = aim_settings::toJson(settings);
  Json::Value wear(Json::objectValue);
  wear["movesToday"] = static_cast<Json::Int64>(aim.movesToday);
  wear["dailyCap"] = static_cast<Json::Int64>(ptzConfig().dailyMoveCap);
  json["wear"] = wear;
  co_return json;
}

drogon::Task<Json::Value> CameraControlFeatureService::updateAim(const CameraAimUpdateInput& input) const
{
  const auto camera = co_await cameraRepository_.findById(input.cameraId);
  if (!camera)
    throw ResponseException(CameraErrors::CameraNotFound);
  const Json::Value before = camera->toJson();
  CameraUpdateInput change;
  change.config = aim_settings::merged({.config = camera->config, .patch = input.patch});
  const auto after = co_await cameraRepository_.update(input.cameraId, change);
  if (const CameraChangeSink* sink = camera_change::getSink())
    co_await sink->publishAudit({.recordId = input.cameraId,
                                 .tableName = TableName::Camera,
                                 .before = before,
                                 .after = after.toJson(),
                                 .actorId = input.actorUserId,
                                 .client = nullptr});
  co_await publishAim(input.cameraId);
  co_return co_await aimState({.cameraId = input.cameraId, .userId = input.actorUserId});
}

drogon::Task<Json::Value> CameraControlFeatureService::lease(const CameraLeaseInput& input) const
{
  const int64_t now = nowMs();
  if (input.hold) {
    co_await admitActor({.userId = input.actorUserId, .role = input.actorRole, .changesPresets = false});
    const PtzClaimResult claim = PtzLease::instance().claim(
        {.cameraId = input.cameraId, .holder = PtzHolder::Viewer, .userId = input.actorUserId, .atMs = now});
    if (claim == PtzClaimResult::Refused)
      throw ResponseException(CameraErrors::CameraControlLocked.withMessage("Someone else is moving the camera"));
  }
  else {
    PtzLease::instance().release(
        {.cameraId = input.cameraId, .holder = PtzHolder::Viewer, .userId = input.actorUserId, .atMs = now});
    PtzLease::instance().release(
        {.cameraId = input.cameraId, .holder = PtzHolder::Person, .userId = input.actorUserId, .atMs = now});
  }
  co_await publishAim(input.cameraId);
  co_return co_await aimState({.cameraId = input.cameraId, .userId = input.actorUserId});
}

drogon::Task<CameraControlResult> CameraControlFeatureService::calibrate(int64_t cameraId) const
{
  co_return co_await onDevice(cameraId, [](ICameraDriver& driver) { return driver.calibrate(); });
}

namespace
{
bool privacyClaimed(const std::string& capabilities)
{
  const Json::Value list = json_util::fromString(capabilities);
  if (!list.isArray())
    return false;
  return std::ranges::any_of(list, [](const Json::Value& name) {
    return name.isString() && name.asString() == "privacy";
  });
}
}

drogon::Task<CameraControlResult>
CameraControlFeatureService::settings(const CameraSettingsInput& input) const
{
  const CameraSettingsDto& body = input.body;
  const int64_t cameraId = input.cameraId;
  const CameraPrivacyState before =
      PrivacyStateService::instance().stateOf(cameraId);
  if (body.privacy)
  {
    CameraRepository repository;
    const auto camera = co_await repository.findById(cameraId);
    if (!camera)
      co_return DriverResult::failed("Camera not found");
    if (!privacyClaimed(camera->capabilities))
    {
      const auto probe = co_await onDevice(
          cameraId, [](ICameraDriver& driver) { return driver.status(); });
      const bool claims =
          probe && probe->ok && probe->data["privacyEnabled"].isBool();
      if (!claims)
        co_return DriverResult{.ok = false,
                                .error = "This camera model does not support "
                                         "the privacy lens mask",
                                .data = Json::Value(),
                                .attempted = true,
                                .failure = "privacy_unsupported"};
    }
  }
  auto result = co_await onDevice(cameraId, [body](ICameraDriver& driver) {
    return driver.settings({.privacy = body.privacy,
                            .led = body.led,
                            .dayNight = body.dayNight,
                            .motion = body.motion,
                            .motionSensitivity = body.motionSensitivity,
                            .autoTrack = body.autoTrack,
                            .alarm = body.alarm,
                            .alarmVolume = body.alarmVolume,
                            .sounding = std::nullopt});
  });
  if (!succeeded(result))
    co_return result;
  if (body.privacy)
  {
    const auto readBack = co_await onDevice(
        cameraId, [](ICameraDriver& driver) { return driver.status(); });
    if (readBack && readBack->ok)
    {
      CameraRepository repository;
      const auto camera = co_await repository.findById(cameraId);
      if (camera)
        co_await PrivacyStateService::instance().refresh(
            {.camera = *camera, .driverStatus = readBack->data, .readable = true});
    }
    co_await recordPrivacyAction(
        input, before, PrivacyStateService::instance().stateOf(cameraId));
  }
  auto& scenes = CameraSceneLog::instance();
  if ((body.privacy && !*body.privacy) || body.dayNight)
    scenes.noteAimed({.cameraId = cameraId, .atMs = nowMs()});
  if (body.privacy)
    co_await announcePrivacy(cameraId);
  else
    co_await announceStatus(cameraId);
  co_return result;
}

drogon::Task<CameraControlResult>
CameraControlFeatureService::speak(int64_t cameraId, const CameraTalkDto& body) const
{
  std::pair<std::vector<int16_t>, int> audio;
  try {
    audio = co_await BlockingTask<std::pair<std::vector<int16_t>, int>>(
        [this, body]() {
          TtsRequest request;
          request.text = body.text;
          request.lang = body.lang == "en" ? TtsLang::EN : TtsLang::ES;
          request.quality = TtsQuality::Auto;
          request.speed = tts_.defaultSpeed();

          const auto floats = tts_.synthesize(request);
          const int rate = tts_.sampleRate();
          std::vector<int16_t> samples;
          samples.reserve(floats.size());
          for (const auto sample : floats) {
            const float clamped = std::max(-1.0F, std::min(1.0F, sample));
            samples.push_back(static_cast<int16_t>(clamped * 32767.0F));
          }
          return std::make_pair(std::move(samples), rate);
        });
  }
  catch (const std::exception& e) {
    co_return DriverResult::failed("Text-to-speech unavailable: " +
                                    std::string(e.what()));
  }

  const auto& [pcm, rate] = audio;
  co_return co_await onDevice(cameraId, [pcm, rate](ICameraDriver& driver) {
    return driver.speak({.samples = pcm, .sampleRate = rate});
  });
}

drogon::Task<std::optional<std::vector<uint8_t>>>
CameraControlFeatureService::masked(int64_t cameraId, std::vector<uint8_t> jpeg) const
{
  const auto rows = co_await zoneRepository_.findByCamera(cameraId);
  std::vector<OperatorZone> zones;
  for (const auto& row : rows) {
    if (row.isEnabled && row.zoneType == ZoneType::Privacy)
      zones.push_back({.cameraId = row.cameraId,
                       .name = row.name,
                       .kind = row.zoneType,
                       .points = parseZonePoints(row.points)});
  }
  if (!privacy_mask::covers(zones))
    co_return std::make_optional(std::move(jpeg));
  co_return co_await BlockingTask<std::optional<std::vector<uint8_t>>>(
      [jpeg = std::move(jpeg), zones = std::move(zones)]() {
        return privacy_mask::maskedJpeg(jpeg, zones);
      },
      BlockingLane::Heavy);
}

drogon::Task<DriverResult> CameraControlFeatureService::freshStill(FrameGrabRequest request) const
{
  auto frame = co_await frames_.grab(request);
  if (!frame || frame->jpeg.empty())
    co_return DriverResult::failed("The camera sent no picture");
  const auto jpeg = co_await masked(request.cameraId, std::move(frame->jpeg));
  if (!jpeg)
    co_return DriverResult::failed("The picture could not be masked");
  co_return stillOf({.jpeg = std::string(jpeg->begin(), jpeg->end()), .atMs = frame->capturedAtMs});
}

drogon::Task<DriverResult> CameraControlFeatureService::sharedStill(FrameGrabRequest request) const
{
  auto ticket = mainStills_.join(request.cameraId);
  if (!ticket.leads())
    co_return co_await ticket.result();
  const DriverResult result = co_await freshStill(std::move(request));
  ticket.land(result);
  co_return result;
}

drogon::Task<CameraControlResult>
CameraControlFeatureService::snapshot(int64_t cameraId, CameraStream stream) const
{
  const auto camera = co_await cameraRepository_.findById(cameraId);
  if (!camera)
    co_return std::nullopt;
  if (!camera->isEnabled)
    co_return DriverResult::failed("This camera is disabled");

  FrameGrabRequest request{.cameraId = cameraId, .cameraName = camera->name, .maxAgeMs = 0, .stream = stream};
  if (!camera_stream_role::isWarm(stream))
    co_return co_await sharedStill(std::move(request));
  if (const auto stored = SnapshotStore::instance().frame(cameraId);
      stored && nowMs() - stored->atMs <= kFreshSnapshotMs)
    co_return stillOf(*stored);
  co_return co_await freshStill(std::move(request));
}

drogon::Task<CameraControlResult>
CameraControlFeatureService::sdDays(int64_t cameraId, const CameraSdDaysDto& query) const
{
  co_return co_await onDevice(cameraId, [query](ICameraDriver& driver) {
    return driver.sdDays({.from = query.from, .to = query.to});
  });
}

drogon::Task<CameraControlResult>
CameraControlFeatureService::sdDay(int64_t cameraId, const CameraSdDayDto& query) const
{
  co_return co_await onDevice(cameraId,
                              [query](ICameraDriver& driver) { return driver.sdDay({.date = query.date}); });
}
