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
  TempDb db{"guard-outbox-adopt-test"};
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

  CameraCommandResult announce(const CameraAnnounceInput& input) const override
  {
    announceCommandIds.push_back(input.commandId);
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

  mutable std::vector<std::string> announceCommandIds;
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

GuardService::Config adoptConfig()
{
  GuardService::Config config;
  config.enabled = true;
  config.profile = "home";
  config.defaultMode = GuardMode::Home;
  config.notifyLevel = 4;
  config.announceLevel = 3;
  config.alarmLevel = 9;
  config.greetEnabled = false;
  config.greetReplyEnabled = false;
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

struct HighObservationInput
{
  std::string eventId;
  int64_t cameraId{0};
  int64_t trackId{0};
};

Json::Value highObservation(const HighObservationInput& input)
{
  Json::Value event(Json::objectValue);
  event["schemaVersion"] = 2;
  event["eventId"] = input.eventId;
  event["cameraId"] = Json::Int64(input.cameraId);
  event["cameraName"] = "front";
  event["rule"] = "person_day";
  event["severity"] = "critical";
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
} // namespace

TEST_CASE("planIntent adopts a legacy-numbered sibling instead of duplicating")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  GuardRepository repository;

  const auto seeded = drogon::sync_wait(repository.planIntent(
      {.commandId = "adp:1:announce:1",
       .encounterId = 0,
       .incidentId = 0,
       .cameraId = 40,
       .personId = 0,
       .kind = "announce",
       .payload = "{\"adopted\":true}",
       .at = 100}));
  REQUIRE(seeded.has_value());

  const auto planned = drogon::sync_wait(repository.planIntent(
      {.commandId = "adp:1:announce:2",
       .encounterId = 0,
       .incidentId = 0,
       .cameraId = 40,
       .personId = 0,
       .kind = "announce",
       .payload = "{\"fresh\":true}",
       .at = 101}));
  REQUIRE(planned.has_value());
  CHECK(planned->commandId == "adp:1:announce:1");
  CHECK(planned->payload == "{\"adopted\":true}");
  CHECK(scalar("SELECT COUNT(*) FROM guard_action_outbox WHERE command_id "
               "LIKE 'adp:1:announce:%'") == "1");
}

TEST_CASE("resume after upgrade dispatches the adopted command once")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  GuardRepository repository;
  REQUIRE(drogon::sync_wait(repository.planIntent(
      {.commandId = "adu:1:announce:1",
       .encounterId = 0,
       .incidentId = 0,
       .cameraId = 41,
       .personId = 0,
       .kind = "announce",
       .payload = "",
       .at = 100})));

  FakeCameraActions camera;
  FakeIdentity identity;
  CountingNotifications notifications;
  GuardService service({.bus = nullptr,
                        .identity = &identity,
                        .notifications = &notifications,
                        .actions = &camera,
                        .assessment = nullptr},
                       adoptConfig());

  REQUIRE(drogon::sync_wait(service.handle(
      highObservation(
          {.eventId = "adu:1", .cameraId = 41, .trackId = 1}),
      1)));
  REQUIRE(camera.announceCommandIds.size() == 1);
  CHECK(camera.announceCommandIds.front() == "adu:1:announce:1");
  CHECK(scalar("SELECT COUNT(*) FROM guard_action_outbox WHERE command_id "
               "LIKE 'adu:1:announce:%'") == "1");
  CHECK(scalar("SELECT status FROM guard_action_outbox WHERE command_id = "
               "'adu:1:announce:1'") == "succeeded");
}

TEST_CASE("listen and greet siblings are never adopted")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  GuardRepository repository;
  REQUIRE(drogon::sync_wait(repository.planIntent(
      {.commandId = "dag:1:greet_listen:101",
       .encounterId = 0,
       .incidentId = 0,
       .cameraId = 42,
       .personId = 0,
       .kind = "greet_listen",
       .payload = "{\"first\":true}",
       .at = 100})));
  const auto second = drogon::sync_wait(repository.planIntent(
      {.commandId = "dag:1:greet_listen:102",
       .encounterId = 0,
       .incidentId = 0,
       .cameraId = 42,
       .personId = 0,
       .kind = "greet_listen",
       .payload = "{\"second\":true}",
       .at = 101}));
  REQUIRE(second.has_value());
  CHECK(second->payload == "{\"second\":true}");
  CHECK(scalar("SELECT COUNT(*) FROM guard_action_outbox WHERE command_id "
               "LIKE 'dag:1:greet_listen:%'") == "2");

  REQUIRE(drogon::sync_wait(repository.planIntent(
      {.commandId = "dag:1:greet:1",
       .encounterId = 0,
       .incidentId = 0,
       .cameraId = 42,
       .personId = 0,
       .kind = "greet",
       .payload = "{\"first\":true}",
       .at = 100})));
  const auto greetSecond = drogon::sync_wait(repository.planIntent(
      {.commandId = "dag:1:greet:2",
       .encounterId = 0,
       .incidentId = 0,
       .cameraId = 42,
       .personId = 0,
       .kind = "greet",
       .payload = "{\"second\":true}",
       .at = 101}));
  REQUIRE(greetSecond.has_value());
  CHECK(greetSecond->payload == "{\"second\":true}");
  CHECK(scalar("SELECT COUNT(*) FROM guard_action_outbox WHERE command_id "
               "LIKE 'dag:1:greet:%'") == "2");
}

TEST_CASE("colon correlations adopt while malformed ids plan normally")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  GuardRepository repository;
  REQUIRE(drogon::sync_wait(repository.planIntent(
      {.commandId = "cam:9:12:notify:1",
       .encounterId = 0,
       .incidentId = 0,
       .cameraId = 9,
       .personId = 0,
       .kind = "notify",
       .payload = "{\"adopted\":true}",
       .at = 100})));
  const auto adopted = drogon::sync_wait(repository.planIntent(
      {.commandId = "cam:9:12:notify:2",
       .encounterId = 0,
       .incidentId = 0,
       .cameraId = 9,
       .personId = 0,
       .kind = "notify",
       .payload = "{\"fresh\":true}",
       .at = 101}));
  REQUIRE(adopted.has_value());
  CHECK(adopted->commandId == "cam:9:12:notify:1");
  CHECK(adopted->payload == "{\"adopted\":true}");

  for (const std::string malformed :
       {"no-colons-at-all", "a:b:x", "a::1", ":b:1"}) {
    const auto planned = drogon::sync_wait(repository.planIntent(
        {.commandId = malformed,
         .encounterId = 0,
         .incidentId = 0,
         .cameraId = 9,
         .personId = 0,
         .kind = "notify",
         .payload = "{\"own\":true}",
         .at = 102}));
    REQUIRE(planned.has_value());
    CHECK(planned->commandId == malformed);
    CHECK(planned->payload == "{\"own\":true}");
  }
}
