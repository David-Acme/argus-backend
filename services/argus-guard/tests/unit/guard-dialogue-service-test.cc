#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <atomic>
#include <camera/camera-action-client.hxx>
#include <chrono>
#include <cstdio>
#include <doctest/doctest.h>
#include <drogon/drogon.h>
#include <guard-schema.hxx>
#include <guard-service.hxx>
#include <identity/identity-client.hxx>
#include <map>
#include <memory>
#include <notification/notification-client.hxx>
#include <optional>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

namespace
{
int tempCounter()
{
  static std::atomic<int> counter{0};
  return counter.fetch_add(1);
}

class TempDb
{
public:
  explicit TempDb(const char* stem)
      : path_(std::string(stem) + "-" + std::to_string(::getpid()) + "-" +
              std::to_string(tempCounter()) + ".db")
  {
  }

  ~TempDb()
  {
    std::remove(path_.c_str());
    std::remove((path_ + "-wal").c_str());
    std::remove((path_ + "-shm").c_str());
  }

  const std::string& path() const { return path_; }

private:
  std::string path_;
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

std::string scalar(const std::string& sql)
{
  const auto rows = DbService::client()->execSqlSync(sql);
  if (rows.empty())
    return {};
  return rows.front()[0].as<std::string>();
}

// Command-id idempotent fake: a replayed command returns the stored response
// without a second capture/announcement, mirroring the camera claim.
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
    const auto existing = announceByCommand.find(input.commandId);
    if (existing != announceByCommand.end())
      return existing->second;
    announced.push_back(input.text);
    CameraCommandResult result = okResult("sent");
    announceByCommand.emplace(input.commandId, result);
    return result;
  }

  CameraCommandResult listen(const CameraListenInput& input) const override
  {
    const auto existing = listenByCommand.find(input.commandId);
    if (existing != listenByCommand.end())
      return existing->second;
    if (input.cameraId != lastCameraId) {
      lastCameraId = input.cameraId;
      nextListen = 0;
    }
    ++listenCalls;
    CameraCommandResult result;
    if (nextListen < transcript.size()) {
      const std::string& text = transcript[nextListen++];
      result = okResult("captured");
      result.captured = !text.empty();
      result.speechDetected = !text.empty();
      result.endpointed = !text.empty();
      result.text = text;
    }
    else {
      result.status = grpc::Status::OK;
      result.outcome = CameraCommandOutcome::INDETERMINATE;
      result.detail = "capture_failed";
    }
    listenByCommand.emplace(input.commandId, result);
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

  mutable std::vector<std::string> announced;
  mutable int listenCalls{0};
  mutable size_t nextListen{0};
  mutable int64_t lastCameraId{0};
  std::vector<std::string> transcript;
  mutable std::map<std::string, CameraCommandResult> announceByCommand;
  mutable std::map<std::string, CameraCommandResult> listenByCommand;
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

class FlakyNotifications final : public NotificationClient
{
public:
  FlakyNotifications()
      : NotificationClient({.target = "127.0.0.1:1", .credential = {}})
  {
  }

  NotificationCreateResult createNotifications(
      const argus::notification::v1::CreateNotificationsRequest& request,
      const argus::sdk::CallerIdentity&) const override
  {
    const int call = calls.fetch_add(1);
    NotificationCreateResult result;
    if (call == 0) {
      result.outcome = NotificationRpcOutcome::Unavailable;
      return result;
    }
    result.outcome = NotificationRpcOutcome::Success;
    result.created = request.user_ids_size();
    return result;
  }

  mutable std::atomic<int> calls{0};
};

Json::Value observation(const std::string& eventId, int64_t cameraId,
                        int64_t trackId)
{
  Json::Value event(Json::objectValue);
  event["schemaVersion"] = 2;
  event["eventId"] = eventId;
  event["cameraId"] = Json::Int64(cameraId);
  event["cameraName"] = "front";
  event["rule"] = "person_day";
  event["severity"] = "info";
  event["escalated"] = false;
  event["trackId"] = Json::Int64(trackId);
  Json::Value object(Json::objectValue);
  object["class"] = "person";
  object["confidence"] = 0.9;
  object["identity"] = "unknown";
  object["trackId"] = Json::Int64(trackId);
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

Json::Value alertObservation(const std::string& eventId, int64_t cameraId,
                             int64_t trackId)
{
  Json::Value event = observation(eventId, cameraId, trackId);
  event["rule"] = "person_in_alert_zone";
  event["severity"] = "critical";
  return event;
}

GuardService::Config dialogueConfig()
{
  GuardService::Config config;
  config.enabled = true;
  config.profile = "home";
  config.defaultMode = GuardMode::Home;
  config.greetEnabled = true;
  config.greetKnown = false;
  config.greetText = "Hola, ¿necesitas algo?";
  config.greetTexts = {"Hola, ¿necesitas algo?"};
  config.greetListenSeconds = 1;
  config.greetReplyEnabled = true;
  config.greetReplyText = "¿En qué te ayudo?";
  config.greetReplyTexts = {"¿En qué te ayudo?"};
  config.greetRepairText = "No te escuché, ¿puedes repetirlo?";
  config.encounterTimeoutS = 300;
  config.crossCameraWindowS = 60;
  config.continuityWindowS = 60;
  config.loiterChecks = 3;
  config.stagingEnabled = false;
  config.maxDialogueTurns = 3;
  return config;
}

std::string encounterGoal(int64_t cameraId)
{
  return scalar(
      "SELECT dialogue_goal FROM guard_encounter WHERE best_camera_id = " +
      std::to_string(cameraId) + " ORDER BY id DESC LIMIT 1");
}

int64_t encounterTurns(int64_t cameraId)
{
  return std::stoll(scalar(
      "SELECT dialogue_turns FROM guard_encounter WHERE best_camera_id = " +
      std::to_string(cameraId) + " ORDER BY id DESC LIMIT 1"));
}
} // namespace

TEST_CASE("every saga failpoint converges on redelivery without duplicates")
{
  const TempDb db("guard-dialogue-service-test");
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, db.path(), "default", -1});
  std::thread runner([] { drogon::app().run(); });
  REQUIRE(waitForBoot(std::chrono::seconds(30)));
  REQUIRE(DbService::runScriptFile(ARGUS_GUARD_SCHEMA_PATH));

  auto failTarget = std::make_shared<std::string>();
  GuardService::Config config = dialogueConfig();
  config.failPoint = [failTarget](const std::string& name) {
    return !failTarget->empty() && *failTarget == name;
  };
  FakeCameraActions camera;
  camera.transcript = {"hola, vengo a ver a Juan", "busco a Juan"};
  FakeIdentity identity;
  CountingNotifications notifications;
  const auto makeService = [&camera, &config, &identity, &notifications]() {
    return std::make_unique<GuardService>(
        GuardService::Dependencies{.bus = nullptr,
                                   .identity = &identity,
                                   .notifications = &notifications,
                                   .actions = &camera,
                                   .assessment = nullptr},
        config);
  };
  auto service = makeService();

  const std::vector<std::string> failpoints = {"after_claim",
                                               "after_incident",
                                               "after_encounter",
                                               "after_dialogue",
                                               "after_assessment",
                                               "after_resolve",
                                               "after_effects",
                                               "after_evidence",
                                               "before_complete",
                                               "after_intent",
                                               "after_effect_rpc",
                                               "after_effect_settle",
                                               "before_challenge_listen",
                                               "after_challenge_listen",
                                               "before_offer_listen",
                                               "after_offer_listen"};

  int cameraId = 10;
  for (const auto& failpoint : failpoints) {
    const std::string eventId = "fp:" + failpoint;
    const std::string eventId2 = eventId + ":2";
    const int64_t cameraRef = cameraId;
    const size_t announcedBefore = camera.announced.size();
    const int listensBefore = camera.listenCalls;

    *failTarget = failpoint;
    bool threw = false;
    try {
      drogon::sync_wait(service->handle(observation(eventId, cameraRef, 1), 1));
    }
    catch (const std::exception&) {
      threw = true;
    }
    CAPTURE(failpoint);
    CHECK(threw);

    *failTarget = "";
    service = makeService();
    REQUIRE(drogon::sync_wait(
        service->handle(observation(eventId, cameraRef, 1), 1)));
    REQUIRE(drogon::sync_wait(
        service->handle(observation(eventId2, cameraRef, 1), 1)));

    CHECK(
        scalar("SELECT status FROM guard_observation_inbox WHERE event_id = '" +
               eventId + "'") == "completed");
    CHECK(
        scalar("SELECT status FROM guard_observation_inbox WHERE event_id = '" +
               eventId2 + "'") == "completed");
    CHECK(scalar("SELECT COUNT(*) FROM guard_incident WHERE event_id IN ('" +
                 eventId + "', '" + eventId2 + "')") == "2");
    CHECK(
        scalar("SELECT COUNT(*) FROM guard_encounter WHERE best_camera_id = " +
               std::to_string(cameraRef)) == "1");
    CAPTURE(failpoint);
    CAPTURE(encounterTurns(cameraRef));
    CAPTURE(scalar("SELECT dialogue_turn_key FROM guard_encounter WHERE "
                   "best_camera_id = " +
                   std::to_string(cameraRef) + " ORDER BY id DESC LIMIT 1"));
    CHECK(encounterGoal(cameraRef) == "done");
    CHECK(encounterTurns(cameraRef) == 3);
    CHECK(camera.listenCalls == listensBefore + 2);
    CHECK(camera.announced.size() == announcedBefore + 2);
    ++cameraId;
  }

  const std::vector<std::string> effectFailpoints = {"after_effects",
                                                     "after_intent",
                                                     "after_effect_rpc",
                                                     "after_effect_settle",
                                                     "before_complete"};
  for (const auto& failpoint : effectFailpoints) {
    const std::string eventId = "fx:" + failpoint;
    const int64_t cameraRef = cameraId;
    *failTarget = failpoint;
    bool threw = false;
    try {
      drogon::sync_wait(
          service->handle(alertObservation(eventId, cameraRef, 1), 1));
    }
    catch (const std::exception&) {
      threw = true;
    }
    CAPTURE(failpoint);
    CHECK(threw);

    *failTarget = "";
    service = makeService();
    REQUIRE(drogon::sync_wait(
        service->handle(alertObservation(eventId, cameraRef, 1), 1)));

    CHECK(
        scalar("SELECT status FROM guard_observation_inbox WHERE event_id = '" +
               eventId + "'") == "completed");
    CHECK(scalar("SELECT COUNT(*) FROM guard_incident WHERE event_id = '" +
                 eventId + "'") == "1");
    CHECK(scalar("SELECT COUNT(*) FROM guard_action WHERE encounter_id > 0 "
                 "AND kind IN ('notify', 'announce', 'alarm', 'siren_arm') "
                 "AND incident_id IN (SELECT id FROM guard_incident "
                 "WHERE event_id = '" +
                 eventId + "')") == "4");
    CHECK(scalar("SELECT COUNT(*) FROM guard_action_outbox WHERE command_id "
                 "LIKE '" +
                 eventId + ":%'") == "4");
    ++cameraId;
  }

  {
    CountingNotifications notifications;
    FakeIdentity identity;
    FakeCameraActions notifyCamera;
    GuardService::Config notifyConfig = dialogueConfig();
    notifyConfig.failPoint = [](const std::string& name) {
      return name == "notify_persist_fail";
    };
    GuardService notifyService(GuardService::Dependencies{.bus = nullptr,
                                                          .identity = &identity,
                                                          .notifications =
                                                              &notifications,
                                                          .actions =
                                                              &notifyCamera,
                                                          .assessment =
                                                              nullptr},
                               notifyConfig);
    const int64_t cameraRef = cameraId;
    REQUIRE(drogon::sync_wait(
        notifyService.handle(alertObservation("np:1", cameraRef, 1), 1)));
    CHECK(notifications.calls.load() == 0);
    CHECK(scalar("SELECT status FROM guard_action_outbox WHERE kind = "
                 "'notify' AND command_id LIKE 'np:1:%'") ==
          "retryable_failed");
    ++cameraId;
  }

  {
    auto flaky = std::make_shared<FlakyNotifications>();
    FakeIdentity flakyIdentity;
    FakeCameraActions flakyCamera;
    GuardService::Config flakyConfig = dialogueConfig();
    flakyConfig.retryBaseMs = 50;
    flakyConfig.retryMaxMs = 200;
    flakyConfig.maxActionsPerHour = 0;
    GuardService flakyService(GuardService::Dependencies{.bus = nullptr,
                                                         .identity =
                                                             &flakyIdentity,
                                                         .notifications =
                                                             flaky.get(),
                                                         .actions =
                                                             &flakyCamera,
                                                         .assessment = nullptr},
                              flakyConfig);
    flakyService.start();
    const int64_t cameraRef = cameraId;
    REQUIRE(drogon::sync_wait(
        flakyService.handle(alertObservation("res:1", cameraRef, 1), 1)));
    CHECK(scalar("SELECT status FROM guard_observation_inbox "
                 "WHERE event_id = 'res:1'") == "processing");
    const std::string resStage = scalar(
        "SELECT stage FROM guard_observation_inbox WHERE event_id = 'res:1'");
    const std::string resNotify =
        scalar("SELECT status FROM guard_action_outbox WHERE kind = 'notify' "
               "AND command_id LIKE 'res:1:%'");
    CAPTURE(resStage);
    CAPTURE(resNotify);
    CAPTURE(flaky->calls.load());
    for (int attempt = 0; attempt < 100; ++attempt) {
      if (scalar("SELECT status FROM guard_observation_inbox "
                 "WHERE event_id = 'res:1'") == "completed")
        break;
      std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    CHECK(scalar("SELECT status FROM guard_observation_inbox "
                 "WHERE event_id = 'res:1'") == "completed");
    CHECK(flaky->calls.load() >= 2);
    CHECK(flakyCamera.announced.size() == 1);
    ++cameraId;
  }

  drogon::app().quit();
  runner.join();
}
