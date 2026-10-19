#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <feature/camera-control/dtos/camera-preset-dto.hxx>
#include <feature/camera-control/dtos/camera-settings-dto.hxx>
#include <feature/camera-control/services/camera-control-feature-service.hxx>
#include <shared/services/camera-driver/camera-driver.hxx>
#include <shared/services/privacy-state/privacy-state.hxx>
#include <sqlite/db-service.hxx>
#include <sync/camera-change-sink.hxx>
#include <sync/resource-change.hxx>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
class AppRunner
{
public:
  AppRunner() : runner_([] { drogon::app().run(); }) {}

  ~AppRunner()
  {
    if (!runner_.joinable())
      return;
    for (int i = 0; i < 3000 && !drogon::app().getLoop()->isRunning(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (drogon::app().getLoop()->isRunning()) {
      drogon::app().quit();
      runner_.join();
      return;
    }
    runner_.detach();
  }

  AppRunner(const AppRunner&) = delete;
  AppRunner& operator=(const AppRunner&) = delete;

private:
  std::thread runner_;
};

bool waitForBoot(std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (drogon::app().isRunning())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return drogon::app().isRunning();
}

class SettingsDriver final : public ICameraDriver
{
public:
  [[nodiscard]] Json::Value capabilities() const override { return {}; }
  DriverResult status() override
  {
    Json::Value data(Json::objectValue);
    if (privacyStatus_)
      data["privacyEnabled"] = *privacyStatus_;
    return {.ok = true, .error = {}, .data = std::move(data)};
  }
  DriverResult presets() override { return {.ok = true, .error = {}, .data = Json::Value()}; }
  DriverResult move(const DriverMoveInput&) override { return {.ok = true}; }
  DriverResult preset(const DriverPresetInput&) override
  {
    if (refuse_)
      return DriverResult::failed("refused");
    return {.ok = true};
  }

  DriverResult settings(const DriverSettingsInput&) override
  {
    if (refuse_)
      return DriverResult::failed("refused");
    return {.ok = true};
  }

  DriverResult speak(const DriverSpeakInput&) override { return {.ok = true}; }

  void refuse(bool value) { refuse_ = value; }

  void privacyStatus(std::optional<bool> value) { privacyStatus_ = value; }

private:
  bool refuse_{false};
  std::optional<bool> privacyStatus_;
};

class RecordingSink final : public CameraChangeSink
{
public:
  [[nodiscard]] drogon::Task<void> emitModule(const ModuleEmitInput&) const override
  {
    co_return;
  }
  [[nodiscard]] drogon::Task<void> publishAudit(const ModuleAuditInput&) const override
  {
    co_return;
  }
  [[nodiscard]] drogon::Task<void>
  publishResource(const CameraResourceChange& input) const override
  {
    notices.push_back(input);
    co_return;
  }

  [[nodiscard]] drogon::Task<void>
  publishAction(const ActionPublishInput& input) const override
  {
    actions.push_back(input.event);
    co_return;
  }

  mutable std::vector<CameraResourceChange> notices;
  mutable std::vector<UserActionEvent> actions;
};

struct Rig
{
  explicit Rig(const char* stem)
      : path(stem), fake(std::make_shared<SettingsDriver>())
  {
    drogon::app().setLogLevel(trantor::Logger::kWarn);
    drogon::app().addDbClient(
        drogon::orm::Sqlite3Config{1, path, "default", -1});
    runner.emplace();
    if (!waitForBoot(std::chrono::seconds(30)))
      throw std::runtime_error("drogon loop did not boot");
    if (!DbService::runScriptFile(ARGUS_CAMERA_SCHEMA_PATH))
      throw std::runtime_error("camera schema apply failed");
    const auto rows = DbService::cameraClient()->execSqlSync(
        "INSERT INTO camera (name, ip, driver) VALUES ('Fake cam', '127.0.0.1', "
        "'rtsp') RETURNING id");
    cameraId = rows.front()["id"].as<int64_t>();
    CameraDriverTestAccess::install(cameraId, fake);
    camera_change::setSink(&sink);
  }

  ~Rig()
  {
    CameraDriverRegistry::instance().forget(cameraId);
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
  }

  std::string path;
  std::optional<AppRunner> runner;
  std::shared_ptr<SettingsDriver> fake;
  RecordingSink sink;
  int64_t cameraId{0};
};
}

Rig* g_rig = nullptr;

Rig& sharedRig()
{
  return *g_rig;
}

TEST_CASE("a settings write announces the camera, and a refused one announces nothing")
{
  Rig& rig = sharedRig();
  rig.sink.notices.clear();
  rig.fake->refuse(false);
  const CameraControlFeatureService service;

  const auto applied = drogon::sync_wait(service.settings(
      {.cameraId = rig.cameraId,
       .body = CameraSettingsDto{.privacy = std::nullopt,
                                 .led = std::nullopt,
                                 .dayNight = std::nullopt,
                                 .motion = std::nullopt,
                                 .motionSensitivity = std::nullopt,
                                 .autoTrack = std::nullopt,
                                 .alarm = true,
                                 .alarmVolume = 100},
       .actorUserId = 7,
       .actorIp = "127.0.0.1"}));
  REQUIRE(applied.has_value());
  REQUIRE(rig.sink.notices.size() == 1);
  CHECK(rig.sink.notices.front().scope == ResourceScope::Status);
  REQUIRE(rig.sink.notices.front().ids.size() == 1);
  CHECK(rig.sink.notices.front().ids.front() == rig.cameraId);

  rig.fake->refuse(true);
  const auto refused = drogon::sync_wait(service.settings(
      {.cameraId = rig.cameraId,
       .body = CameraSettingsDto{.privacy = std::nullopt,
                                 .led = std::nullopt,
                                 .dayNight = std::nullopt,
                                 .motion = std::nullopt,
                                 .motionSensitivity = std::nullopt,
                                 .autoTrack = std::nullopt,
                                 .alarm = false,
                                 .alarmVolume = std::nullopt}}));
  if (!refused) {
    FAIL("expected the control service to answer");
    return;
  }
  CHECK_FALSE(refused->ok);
  CHECK(rig.sink.notices.size() == 1);
}

TEST_CASE("a privacy toggle is audited with its actor and announces status and overview")
{
  Rig& rig = sharedRig();
  rig.sink.notices.clear();
  rig.sink.actions.clear();
  rig.fake->refuse(false);
  rig.fake->privacyStatus(true);
  DbService::cameraClient()->execSqlSync(
      "UPDATE camera SET capabilities = '[\"privacy\"]' WHERE id = ?",
      rig.cameraId);
  PrivacyStateService::instance().forget(rig.cameraId);
  const CameraControlFeatureService service;

  const auto masked = drogon::sync_wait(service.settings(
      {.cameraId = rig.cameraId,
       .body = CameraSettingsDto{.privacy = true,
                                 .led = std::nullopt,
                                 .dayNight = std::nullopt,
                                 .motion = std::nullopt,
                                 .motionSensitivity = std::nullopt,
                                 .autoTrack = std::nullopt,
                                 .alarm = std::nullopt,
                                 .alarmVolume = std::nullopt},
       .actorUserId = 41,
       .actorIp = "192.0.2.7"}));
  REQUIRE(masked.has_value());
  CHECK(masked->ok);
  CHECK(PrivacyStateService::instance().stateOf(rig.cameraId) ==
        CameraPrivacyState::On);
  REQUIRE(rig.sink.actions.size() == 1);
  CHECK(rig.sink.actions.front().userId == 41);
  CHECK(rig.sink.actions.front().recordId == rig.cameraId);
  CHECK(rig.sink.actions.front().action == UserAction::Update);
  CHECK(rig.sink.actions.front().ipAddress == "192.0.2.7");
  CHECK(rig.sink.actions.front().newData["privacyState"] == "on");
  CHECK(rig.sink.actions.front().newData["requested"].asBool());
  const auto counts = [&rig](ResourceScope scope) {
    return std::ranges::count_if(rig.sink.notices, [scope](const auto& notice) {
      return notice.scope == scope;
    });
  };
  CHECK(counts(ResourceScope::Status) >= 1);
  CHECK(counts(ResourceScope::Overview) >= 1);
  CHECK(rig.sink.notices.back().scope == ResourceScope::Overview);

  rig.fake->privacyStatus(false);
  const auto cleared = drogon::sync_wait(service.settings(
      {.cameraId = rig.cameraId,
       .body = CameraSettingsDto{.privacy = false,
                                 .led = std::nullopt,
                                 .dayNight = std::nullopt,
                                 .motion = std::nullopt,
                                 .motionSensitivity = std::nullopt,
                                 .autoTrack = std::nullopt,
                                 .alarm = std::nullopt,
                                 .alarmVolume = std::nullopt},
       .actorUserId = 41,
       .actorIp = "192.0.2.7"}));
  REQUIRE(cleared.has_value());
  CHECK(cleared->ok);
  CHECK(PrivacyStateService::instance().stateOf(rig.cameraId) ==
        CameraPrivacyState::Off);
  REQUIRE(rig.sink.actions.size() == 2);
  CHECK(counts(ResourceScope::Overview) >= 2);
  CHECK(rig.sink.actions.back().oldData["privacyState"] == "on");
  CHECK(rig.sink.actions.back().newData["privacyState"] == "off");
  CHECK_FALSE(rig.sink.actions.back().newData["requested"].asBool());
}

TEST_CASE("saving or deleting a preset announces, and moving to one does not"){
  Rig& rig = sharedRig();
  rig.sink.notices.clear();
  rig.fake->refuse(false);
  const CameraControlFeatureService service;

  const auto saved = drogon::sync_wait(service.preset(
      {.cameraId = rig.cameraId, .body = CameraPresetDto{.action = "save", .id = "", .name = "Gate"}, .actorUserId = 1}));
  REQUIRE(saved.has_value());
  CHECK(rig.sink.notices.size() == 1);

  const auto moved = drogon::sync_wait(service.preset(
      {.cameraId = rig.cameraId, .body = CameraPresetDto{.action = "goto", .id = "1", .name = ""}, .actorUserId = 1}));
  REQUIRE(moved.has_value());
  CHECK(std::ranges::count(rig.sink.notices, ResourceScope::Status, &CameraResourceChange::scope) == 1);

  const auto removed = drogon::sync_wait(service.preset(
      {.cameraId = rig.cameraId, .body = CameraPresetDto{.action = "delete", .id = "1", .name = ""}, .actorUserId = 1}));
  REQUIRE(removed.has_value());
  REQUIRE(std::ranges::count(rig.sink.notices, ResourceScope::Status, &CameraResourceChange::scope) == 2);
  CHECK(rig.sink.notices.back().ids.front() == rig.cameraId);
}

int main(int argc, char** argv)
{
  try {
    Rig rig("camera-status-announce-test.db");
    g_rig = &rig;
    doctest::Context context(argc, argv);
    return context.run();
  }
  catch (const std::exception& error) {
    LOG_FATAL << "the fixture did not start: " << error.what();
    return 1;
  }
}
