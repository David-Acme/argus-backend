#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <atomic>
#include <camera/camera-action-client.hxx>
#include <chrono>
#include <cstdio>
#include <doctest/doctest.h>
#include <drogon/drogon.h>
#include <guard-repository.hxx>
#include <guard-schema.hxx>
#include <guard-service.hxx>
#include <identity/identity-client.hxx>
#include <notification/notification-client.hxx>
#include <optional>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "temp-db.hxx"
#include "wait-for-boot.hxx"

using guard_test::TempDb;
using guard_test::waitForBoot;

namespace
{
std::string scalar(const std::string& sql)
{
  const auto rows = DbService::client()->execSqlSync(sql);
  if (rows.empty())
    return {};
  return rows.front()[0].as<std::string>();
}

struct SharedBoot
{
  TempDb db{"guard-belief-scope-test"};
  std::thread runner;

  SharedBoot()
  {
    drogon::app().setLogLevel(trantor::Logger::kWarn);
    drogon::app().addDbClient(
        drogon::orm::Sqlite3Config{1, db.path(), "default", -1});
    runner = std::thread([] { drogon::app().run(); });
    if (!waitForBoot(std::chrono::seconds(30)))
      throw std::runtime_error("drogon loop did not boot");
    if (!DbService::runScriptFile(ARGUS_GUARD_SCHEMA_PATH))
      throw std::runtime_error("guard schema apply failed");
  }

  ~SharedBoot()
  {
    drogon::app().quit();
    if (runner.joinable())
      runner.join();
  }
};

SharedBoot& sharedBoot()
{
  static SharedBoot boot;
  return boot;
}

CameraCommandResult okResult(std::string detail)
{
  CameraCommandResult result;
  result.status = grpc::Status::OK;
  result.outcome = CameraCommandOutcome::SUCCEEDED;
  result.detail = std::move(detail);
  return result;
}

class FakeCameraActions final : public CameraActionClient
{
public:
  FakeCameraActions()
      : CameraActionClient({.target = "127.0.0.1:1", .credential = {}})
  {
  }

  CameraCommandResult announce(const CameraAnnounceInput&) const override
  {
    announceCalls.fetch_add(1);
    return okResult("sent");
  }

  CameraCommandResult listen(const CameraListenInput&) const override
  {
    CameraCommandResult result;
    result.status = grpc::Status::OK;
    result.outcome = CameraCommandOutcome::INDETERMINATE;
    result.detail = "capture_failed";
    return result;
  }

  CameraCommandResult alarm(const CameraAlarmInput&) const override
  {
    alarmCalls.fetch_add(1);
    return okResult("alarmed");
  }

  CameraCommandResult setSiren(const CameraSirenInput&) const override
  {
    CameraCommandResult result;
    result.status = grpc::Status::OK;
    result.outcome = CameraCommandOutcome::REJECTED;
    result.detail = "siren_off";
    return result;
  }

  std::optional<CameraCrop>
  personCrop(const CameraPersonCropInput&) const override
  {
    return std::nullopt;
  }

  mutable std::atomic<int> announceCalls{0};
  mutable std::atomic<int> alarmCalls{0};
};

class FakeIdentity final : public IdentityClient
{
public:
  FakeIdentity() : IdentityClient("127.0.0.1:1", "fleet") {}

  std::optional<std::vector<int64_t>> listNotifiableUsers() const override
  {
    return std::vector<int64_t>{7};
  }
};

class CountingNotifications final : public NotificationClient
{
public:
  CountingNotifications()
      : NotificationClient({.target = "127.0.0.1:1", .credential = {}})
  {
  }

  NotificationCreateResult createNotifications(
      const argus::notification::v1::CreateNotificationsRequest& request,
      const argus::sdk::CallerIdentity&) const override
  {
    calls.fetch_add(1);
    NotificationCreateResult result;
    result.outcome = NotificationRpcOutcome::Success;
    result.created = request.user_ids_size();
    return result;
  }

  mutable std::atomic<int> calls{0};
};

struct ScopeObservationInput
{
  std::string eventId;
  int64_t cameraId{0};
  int64_t trackId{0};
  std::string rule;
  std::string severity;
};

Json::Value scopeObservation(const ScopeObservationInput& input)
{
  Json::Value event(Json::objectValue);
  event["schemaVersion"] = 2;
  event["eventId"] = input.eventId;
  event["cameraId"] = Json::Int64(input.cameraId);
  event["cameraName"] = "front";
  event["rule"] = input.rule;
  event["severity"] = input.severity;
  event["escalated"] = false;
  event["trackId"] = Json::Int64(input.trackId);
  Json::Value object(Json::objectValue);
  object["class"] = "person";
  object["confidence"] = 0.9;
  object["identity"] = "unknown";
  object["trackId"] = Json::Int64(input.trackId);
  object["personId"] = Json::Int64(0);
  Json::Value bbox(Json::objectValue);
  bbox["x"] = 0.0;
  bbox["y"] = 0.0;
  bbox["w"] = 40.0;
  bbox["h"] = 40.0;
  object["bbox"] = bbox;
  Json::Value objects(Json::arrayValue);
  objects.append(object);
  event["objects"] = objects;
  return event;
}

GuardService::Config scopeConfig()
{
  GuardService::Config config;
  config.enabled = true;
  config.profile = "home";
  config.defaultMode = GuardMode::Home;
  config.decisionMode = "enforce";
  config.greetEnabled = false;
  config.greetReplyEnabled = false;
  config.announceLevel = 3;
  config.alarmLevel = 4;
  config.actionCooldownS = 0;
  config.maxActionsPerHour = 1000;
  config.encounterTimeoutS = 300;
  config.crossCameraWindowS = 60;
  config.continuityWindowS = 60;
  config.loiterChecks = 3;
  config.stagingEnabled = false;
  config.maxDialogueTurns = 3;
  return config;
}

struct ScopeHarness
{
  FakeCameraActions camera;
  FakeIdentity identity;
  CountingNotifications notifications;
  GuardService::Config config = scopeConfig();

  std::unique_ptr<GuardService> makeService()
  {
    return std::make_unique<GuardService>(
        GuardService::Dependencies{.bus = nullptr,
                                   .identity = &identity,
                                   .notifications = &notifications,
                                   .actions = &camera,
                                   .assessment = nullptr},
        config);
  }
};

std::string journalField(const std::string& eventId, const std::string& column)
{
  return scalar("SELECT " + column + " FROM guard_decision_journal WHERE "
                "event_id = '" +
                eventId + "'");
}
} // namespace

TEST_CASE("scope notify suppresses only the notification")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ScopeHarness harness;
  harness.config.beliefGateScope = BeliefGateScope::Notify;
  auto service = harness.makeService();

  REQUIRE(drogon::sync_wait(service->handle(
      scopeObservation({.eventId = "gsn:1",
                        .cameraId = 30,
                        .trackId = 1,
                        .rule = "person_in_alert_zone",
                        .severity = "critical"}),
      1)));
  CHECK(harness.notifications.calls == 0);
  CHECK(harness.camera.announceCalls == 1);
  CHECK(harness.camera.alarmCalls == 1);
  CHECK(journalField("gsn:1", "suppression_reason") == "belief_gate");
  CHECK(journalField("gsn:1", "suppressed_kinds") == "[\"notify\"]");
  CHECK(journalField("gsn:1", "did_notify") == "0");
  CHECK(journalField("gsn:1", "legacy_would_notify") == "1");
}

TEST_CASE("scope communication suppresses notify and announce")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ScopeHarness harness;
  harness.config.beliefGateScope = BeliefGateScope::Communication;
  auto service = harness.makeService();

  REQUIRE(drogon::sync_wait(service->handle(
      scopeObservation({.eventId = "gsc:1",
                        .cameraId = 31,
                        .trackId = 1,
                        .rule = "person_in_alert_zone",
                        .severity = "critical"}),
      1)));
  CHECK(harness.notifications.calls == 0);
  CHECK(harness.camera.announceCalls == 0);
  CHECK(harness.camera.alarmCalls == 1);
  CHECK(journalField("gsc:1", "suppression_reason") == "belief_gate");
  CHECK(journalField("gsc:1", "suppressed_kinds") ==
        "[\"notify\",\"announce\"]");
}

TEST_CASE("scope all without hard floor suppresses every effect")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ScopeHarness harness;
  harness.config.beliefGateScope = BeliefGateScope::All;
  harness.config.announceLevel = 2;
  harness.config.alarmLevel = 2;
  auto service = harness.makeService();

  REQUIRE(drogon::sync_wait(service->handle(
      scopeObservation({.eventId = "gsa:1",
                        .cameraId = 32,
                        .trackId = 1,
                        .rule = "person_day",
                        .severity = "info"}),
      1)));
  CHECK(harness.notifications.calls == 0);
  CHECK(harness.camera.announceCalls == 0);
  CHECK(harness.camera.alarmCalls == 0);
  CHECK(journalField("gsa:1", "suppression_reason") == "belief_gate");
  CHECK(journalField("gsa:1", "suppressed_kinds") ==
        "[\"notify\",\"announce\",\"alarm\",\"siren_arm\"]");
  CHECK(journalField("gsa:1", "did_notify") == "0");
}

TEST_CASE("a hard floor lets physical effects through under scope all")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ScopeHarness harness;
  harness.config.beliefGateScope = BeliefGateScope::All;
  auto service = harness.makeService();

  REQUIRE(drogon::sync_wait(service->handle(
      scopeObservation({.eventId = "gsh:1",
                        .cameraId = 33,
                        .trackId = 1,
                        .rule = "person_in_alert_zone",
                        .severity = "critical"}),
      1)));
  CHECK(harness.notifications.calls == 0);
  CHECK(harness.camera.announceCalls == 0);
  CHECK(harness.camera.alarmCalls == 1);
  CHECK(journalField("gsh:1", "suppression_reason") == "belief_gate");
  CHECK(journalField("gsh:1", "suppressed_kinds") ==
        "[\"notify\",\"announce\"]");
}

TEST_CASE("shadow mode never suppresses by belief regardless of scope")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ScopeHarness harness;
  harness.config.decisionMode = "shadow";
  harness.config.beliefGateScope = BeliefGateScope::All;
  auto service = harness.makeService();

  REQUIRE(drogon::sync_wait(service->handle(
      scopeObservation({.eventId = "gss:1",
                        .cameraId = 34,
                        .trackId = 1,
                        .rule = "person_in_alert_zone",
                        .severity = "critical"}),
      1)));
  CHECK(harness.notifications.calls == 1);
  CHECK(harness.camera.announceCalls == 1);
  CHECK(harness.camera.alarmCalls == 1);
  CHECK(journalField("gss:1", "suppression_reason") == "none");
  CHECK(journalField("gss:1", "suppressed_kinds") == "[]");
  CHECK(journalField("gss:1", "did_notify") == "1");
}

TEST_CASE("belief config resolves once per refresh window")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ConfigService::setRuntimeString("guard.belief.camera.63.threshold_medium",
                                  "9");
  ScopeHarness harness;
  harness.config.beliefRefreshS = 3600;
  auto service = harness.makeService();

  REQUIRE(drogon::sync_wait(service->handle(
      scopeObservation({.eventId = "gbc:1",
                        .cameraId = 63,
                        .trackId = 1,
                        .rule = "person_day",
                        .severity = "info"}),
      1)));
  CHECK(journalField("gbc:1", "belief_threshold") == "9");

  ScopeHarness medium;
  medium.config = harness.config;
  auto mediumService = medium.makeService();
  REQUIRE(drogon::sync_wait(mediumService->handle(
      scopeObservation({.eventId = "gbc:2",
                        .cameraId = 63,
                        .trackId = 2,
                        .rule = "person_day",
                        .severity = "info"}),
      1)));
  CHECK(journalField("gbc:2", "belief_threshold") == "9");

  ConfigService::setRuntimeString("guard.belief.camera.63.threshold_medium",
                                  "1");
  REQUIRE(drogon::sync_wait(service->handle(
      scopeObservation({.eventId = "gbc:3",
                        .cameraId = 63,
                        .trackId = 3,
                        .rule = "person_day",
                        .severity = "info"}),
      1)));
  CHECK(journalField("gbc:3", "belief_threshold") == "9");

  ScopeHarness fresh;
  fresh.config = harness.config;
  fresh.config.beliefRefreshS = 0;
  auto freshService = fresh.makeService();
  REQUIRE(drogon::sync_wait(freshService->handle(
      scopeObservation({.eventId = "gbc:4",
                        .cameraId = 63,
                        .trackId = 4,
                        .rule = "person_day",
                        .severity = "info"}),
      1)));
  CHECK(journalField("gbc:4", "belief_threshold") == "1");
}
