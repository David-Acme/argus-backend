#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <atomic>
#include <camera/camera-action-client.hxx>
#include <condition_variable>
#include <mutex>
#include <runtime/blocking-task.hxx>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <doctest/doctest.h>
#include <test-support/app-runner.hxx>
#include <drogon/drogon.h>
#include <functional>
#include <feature/guard/services/guard-feature-service.hxx>
#include <feature/guard/guard-repository.hxx>
#include <feature/guard/guard-schema.hxx>
#include <feature/guard/guard-service.hxx>
#include <identity/identity-client.hxx>
#include <memory>
#include <notification/notification-client.hxx>
#include <optional>
#include <sqlite/db-service.hxx>
#include <text/json-util.hxx>
#include <stdexcept>
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
using test_support::AppRunner;

std::string scalar(const std::string& sql)
{
  const auto rows = DbService::client()->execSqlSync(sql);
  if (rows.empty())
    return {};
  return rows.front()[0].as<std::string>();
}

struct SharedBoot
{
  TempDb db{"guard-notify-thread-test"};
  std::optional<AppRunner> runner;

  SharedBoot()
  {
    drogon::app().setLogLevel(trantor::Logger::kWarn);
    drogon::app().addDbClient(
        drogon::orm::Sqlite3Config{1, db.path(), "default", -1});
    runner.emplace();
    if (!waitForBoot(std::chrono::seconds(30)))
      throw std::runtime_error("drogon loop did not boot");
    if (!DbService::runScriptFile(ARGUS_GUARD_SCHEMA_PATH))
      throw std::runtime_error("guard schema apply failed");
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

struct CapturedNotification
{
  std::string title;
  std::string body;
  std::string data;
};

class CapturingNotifications final : public NotificationClient
{
public:
  CapturingNotifications()
      : NotificationClient({.target = "127.0.0.1:1", .credential = {}})
  {
  }

  NotificationCreateResult createNotifications(
      const argus::notification::v1::CreateNotificationsRequest& request,
      const argus::client::CallerIdentity&) const override
  {
    calls.fetch_add(1);
    CapturedNotification captured;
    captured.title = request.title();
    captured.body = request.body();
    captured.data = request.data();
    sent.push_back(std::move(captured));
    NotificationCreateResult result;
    result.outcome = NotificationRpcOutcome::Success;
    result.created = request.user_ids_size();
    return result;
  }

  mutable std::atomic<int> calls{0};
  mutable std::vector<CapturedNotification> sent;
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
      const argus::client::CallerIdentity&) const override
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

class ThrowingNotifications final : public NotificationClient
{
public:
  ThrowingNotifications()
      : NotificationClient({.target = "127.0.0.1:1", .credential = {}})
  {
  }

  NotificationCreateResult createNotifications(
      const argus::notification::v1::CreateNotificationsRequest&,
      const argus::client::CallerIdentity&) const override
  {
    calls.fetch_add(1);
    throw std::runtime_error("simulated crash during notification RPC");
  }

  mutable std::atomic<int> calls{0};
};

struct ThreadObservationInput
{
  std::string eventId;
  int64_t cameraId{0};
  int64_t trackId{0};
  std::string rule;
  std::string severity;
  std::string zoneKind;
  std::string signature;
};

Json::Value threadObservation(const ThreadObservationInput& input)
{
  Json::Value event(Json::objectValue);
  event["schemaVersion"] = 3;
  event["eventId"] = input.eventId;
  event["cameraId"] = Json::Int64(input.cameraId);
  event["cameraName"] = "front";
  event["rule"] = input.rule;
  event["severity"] = input.severity;
  event["escalated"] = false;
  event["trackId"] = Json::Int64(input.trackId);
  event["publishedAt"] = Json::Int64(1700000000000);
  Json::Value object(Json::objectValue);
  object["class"] = "person";
  object["confidence"] = 0.9;
  object["identity"] = "unknown";
  object["identityState"] = "unrecognized";
  object["identifyAttempts"] = 2;
  object["scoreMedian"] = 0.9;
  object["scoreSamples"] = 4;
  object["zoneWindows"] = 3;
  object["trackWindows"] = 4;
  object["areaSpread"] = 1.2;
  object["trackId"] = Json::Int64(input.trackId);
  object["firstSeenMs"] = Json::Int64(1700000000000 - 18000);
  object["lastSeenMs"] = Json::Int64(1700000000000);
  object["dwellMs"] = Json::Int64(18000);
  object["zoneKind"] = input.zoneKind;
  object["observationId"] = "20:1:1700000000000";
  if (!input.signature.empty())
    object["signature"] = input.signature;
  object["bbox"] = Json::Value(Json::objectValue);
  Json::Value objects(Json::arrayValue);
  objects.append(object);
  event["objects"] = objects;
  return event;
}

GuardService::Config threadConfig()
{
  GuardService::Config config;
  config.staleObservationS = 0;
  config.enabled = true;
  config.profile = "home";
  config.defaultMode = GuardMode::Home;
  config.greetEnabled = false;
  config.greetReplyEnabled = false;
  config.announceLevel = 9;
  config.alarmLevel = 9;
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

struct ThreadHarness
{
  FakeCameraActions camera;
  FakeIdentity identity;
  CapturingNotifications notifications;
  std::shared_ptr<std::string> failTarget =
      std::make_shared<std::string>();
  GuardService::Config config = threadConfig();
  GuardAssessment* assessment{nullptr};

  std::unique_ptr<GuardService> makeService()
  {
    GuardService::Config serviceConfig = config;
    serviceConfig.failPoint = [failTarget = failTarget](
                                  const std::string& name) {
      return !failTarget->empty() && *failTarget == name;
    };
    return std::make_unique<GuardService>(
        GuardService::Dependencies{.bus = nullptr,
                                   .identity = &identity,
                                   .notifications = &notifications,
                                   .actions = &camera,
                                   .assessment = assessment},
        serviceConfig);
  }
};

class ScriptedAssessment final : public GuardAssessment
{
public:
  ScriptedAssessment(const CapturingNotifications& notifications,
                     std::vector<std::string> tags, bool veto = false)
      : GuardAssessment({.camera = nullptr, .vlm = nullptr, .llm = nullptr},
                        GuardAssessmentConfig{}),
        notifications_(notifications), tags_(std::move(tags)), veto_(veto),
        threat_(veto ? "none" : "high")
  {
  }

  ScriptedAssessment(const CapturingNotifications& notifications,
                     std::vector<std::string> tags, bool veto, std::string threat)
      : GuardAssessment({.camera = nullptr, .vlm = nullptr, .llm = nullptr},
                        GuardAssessmentConfig{}),
        notifications_(notifications), tags_(std::move(tags)), veto_(veto),
        threat_(std::move(threat))
  {
  }

  drogon::Task<GuardAssessmentResult>
  assess(const GuardAssessmentInput&) const override
  {
    notificationsBefore = notifications_.calls;
    ++calls;
    GuardAssessmentResult result;
    result.performed = true;
    result.valid = true;
    result.threat = threat_;
    result.veto = veto_;
    result.tags = tags_;
    co_return result;
  }

  mutable int calls{0};
  mutable int notificationsBefore{-1};

private:
  const CapturingNotifications& notifications_;
  std::vector<std::string> tags_;
  bool veto_{false};
  std::string threat_;
};

class GatedAssessment final : public GuardAssessment
{
public:
  GatedAssessment()
      : GuardAssessment({.camera = nullptr, .vlm = nullptr, .llm = nullptr},
                        GuardAssessmentConfig{})
  {
  }

  drogon::Task<GuardAssessmentResult>
  assess(const GuardAssessmentInput&) const override
  {
    entered.fetch_add(1);
    co_await BlockingTask<bool>{[this]() {
      std::unique_lock lock(mutex_);
      gate_.wait(lock, [this] { return open_; });
      return true;
    }};
    GuardAssessmentResult result;
    result.performed = true;
    result.valid = true;
    result.threat = "none";
    co_return result;
  }

  void open() const
  {
    {
      std::scoped_lock lock(mutex_);
      open_ = true;
    }
    gate_.notify_all();
  }

  mutable std::atomic<int> entered{0};

private:
  mutable std::mutex mutex_;
  mutable std::condition_variable gate_;
  mutable bool open_{false};
};

std::string threadField(int64_t cameraId, const std::string& column)
{
  return scalar("SELECT " + column + " FROM guard_encounter WHERE "
                "best_camera_id = " +
                std::to_string(cameraId));
}

std::string journalField(const std::string& eventId, const std::string& column)
{
  return scalar("SELECT " + column + " FROM guard_decision_journal WHERE "
                "event_id = '" +
                eventId + "'");
}

int64_t testNowMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::system_clock::now().time_since_epoch())
      .count();
}
}

TEST_CASE("first crossing notifies with a deterministic body")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ThreadHarness harness;
  auto service = harness.makeService();

  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "nt:1",
                         .cameraId = 20,
                         .trackId = 1,
                         .rule = "person_in_alert_zone",
                         .severity = "critical",
                         .zoneKind = "alert", .signature = {}}),
      1)));
  CHECK(harness.notifications.calls == 1);
  REQUIRE(harness.notifications.sent.size() == 1);
  CHECK(harness.notifications.sent.front().title ==
        "Persona desconocida \u00b7 front");
  CHECK(harness.notifications.sent.front().body ==
        "En la zona de alerta, desde hace 18 s. Argus sigue observando.");
  const Json::Value data =
      json_util::fromString(harness.notifications.sent.front().data);
  CHECK(data["zoneKind"].asString() == "alert");
  CHECK(data["dwellS"].asInt64() == 18);
  CHECK(data["identityState"].asString() == "unrecognized");
  CHECK(data["cameraName"].asString() == "front");
  CHECK(data["kind"].asString() == "guard_episode");
  CHECK(data["phase"].asString() == "opened");
  CHECK(data["urgency"].asString() == "critical");
  CHECK(data["action"].asString() == "watching");
  CHECK(data["lang"].asString() == "es");
  CHECK(data["episodeId"].asInt64() == data["encounterId"].asInt64());
  CHECK(data["threadKey"].asString() ==
        "guard:episode:" + std::to_string(data["encounterId"].asInt64()));
  REQUIRE(data["reasons"].isArray());
  CHECK(data["reasons"][0].asString() == "alert_zone");
  CHECK(threadField(20, "notify_count") == "1");
  CHECK(threadField(20, "notify_highest_rank") == "4");
}

TEST_CASE("same-tier repeat is suppressed and journaled in shadow mode")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ThreadHarness harness;
  harness.config.decisionMode = "shadow";
  auto service = harness.makeService();

  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "ntr:1",
                         .cameraId = 22,
                         .trackId = 1,
                         .rule = "person_in_alert_zone",
                         .severity = "critical",
                         .zoneKind = "alert", .signature = {}}),
      1)));
  CHECK(harness.notifications.calls == 1);

  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "ntr:2",
                         .cameraId = 22,
                         .trackId = 1,
                         .rule = "person_in_alert_zone",
                         .severity = "critical",
                         .zoneKind = "alert", .signature = {}}),
      1)));
  CHECK(harness.notifications.calls == 1);
  CHECK(journalField("ntr:2", "suppression_reason") == "thread_suppressed");
  CHECK(journalField("ntr:2", "decision_mode") == "shadow");
  CHECK(journalField("ntr:2", "legacy_would_notify") == "1");
  CHECK(journalField("ntr:2", "did_notify") == "0");
}

TEST_CASE("a strict tier increase notifies exactly once more")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ThreadHarness harness;
  auto service = harness.makeService();

  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "nti:1",
                         .cameraId = 23,
                         .trackId = 1,
                         .rule = "person_day",
                         .severity = "info",
                         .zoneKind = "monitor", .signature = {}}),
      1)));
  CHECK(harness.notifications.calls == 1);
  CHECK(threadField(23, "notify_highest_rank") == "2");

  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "nti:2",
                         .cameraId = 23,
                         .trackId = 1,
                         .rule = "person_in_alert_zone",
                         .severity = "critical",
                         .zoneKind = "alert", .signature = {}}),
      1)));
  CHECK(harness.notifications.calls == 2);
  CHECK(threadField(23, "notify_highest_rank") == "4");

  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "nti:3",
                         .cameraId = 23,
                         .trackId = 1,
                         .rule = "person_in_alert_zone",
                         .severity = "critical",
                         .zoneKind = "alert", .signature = {}}),
      1)));
  CHECK(harness.notifications.calls == 2);
  CHECK(journalField("nti:3", "suppression_reason") == "thread_suppressed");
}

TEST_CASE("thread state survives restart and saga resume")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ThreadHarness harness;
  auto service = harness.makeService();

  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "nts:1",
                         .cameraId = 24,
                         .trackId = 1,
                         .rule = "person_in_alert_zone",
                         .severity = "critical",
                         .zoneKind = "alert", .signature = {}}),
      1)));
  CHECK(harness.notifications.calls == 1);

  GuardService restarted({.bus = nullptr,
                          .identity = &harness.identity,
                          .notifications = &harness.notifications,
                          .actions = &harness.camera,
                          .assessment = nullptr},
                         harness.config);
  REQUIRE(drogon::sync_wait(restarted.handle(
      threadObservation({.eventId = "nts:1",
                         .cameraId = 24,
                         .trackId = 1,
                         .rule = "person_in_alert_zone",
                         .severity = "critical",
                         .zoneKind = "alert", .signature = {}}),
      2)));
  CHECK(harness.notifications.calls == 1);
  CHECK(scalar("SELECT COUNT(*) FROM guard_decision_journal WHERE event_id = "
               "'nts:1'") == "1");
  CHECK(threadField(24, "notify_count") == "1");
}

TEST_CASE("persist-then-fail retries to success and records the thread once")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  FakeCameraActions camera;
  FakeIdentity identity;
  FlakyNotifications notifications;
  ThreadHarness harness;
  auto service = std::make_unique<GuardService>(
      GuardService::Dependencies{.bus = nullptr,
                                 .identity = &identity,
                                 .notifications = &notifications,
                                 .actions = &camera,
                                 .assessment = nullptr},
      harness.config);

  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "ntf:1",
                         .cameraId = 26,
                         .trackId = 1,
                         .rule = "person_in_alert_zone",
                         .severity = "critical",
                         .zoneKind = "alert", .signature = {}}),
      1)));
  CHECK(notifications.calls == 1);
  CHECK(threadField(26, "notify_count") == "0");

  DbService::client()->execSqlSync(
      "UPDATE guard_action_outbox SET next_attempt_at = 0 WHERE command_id = "
      "'ntf:1:notify:1'");

  REQUIRE(drogon::sync_wait(service->handleLocalRetry(
      threadObservation({.eventId = "ntf:1",
                         .cameraId = 26,
                         .trackId = 1,
                         .rule = "person_in_alert_zone",
                         .severity = "critical",
                         .zoneKind = "alert",
                         .signature = {}}))));
  CHECK(notifications.calls == 2);
  CHECK(threadField(26, "notify_count") == "1");
  CHECK(threadField(26, "notify_highest_rank") == "4");
  CHECK(journalField("ntf:1", "did_notify") == "1");

  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "ntf:2",
                         .cameraId = 26,
                         .trackId = 1,
                         .rule = "person_in_alert_zone",
                         .severity = "critical",
                         .zoneKind = "alert", .signature = {}}),
      1)));
  CHECK(notifications.calls == 2);
  CHECK(journalField("ntf:2", "suppression_reason") == "thread_suppressed");
}

TEST_CASE("a thread-suppressed pass still runs announce and alarm")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ThreadHarness harness;
  harness.config.announceLevel = 3;
  harness.config.alarmLevel = 4;
  harness.config.defaultMode = GuardMode::Armed;
  auto service = harness.makeService();

  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "nta:1",
                         .cameraId = 27,
                         .trackId = 1,
                         .rule = "person_in_alert_zone",
                         .severity = "critical",
                         .zoneKind = "alert", .signature = {}}),
      1)));
  CHECK(harness.notifications.calls == 1);
  CHECK(harness.camera.announceCalls == 1);
  CHECK(harness.camera.alarmCalls == 1);

  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "nta:2",
                         .cameraId = 27,
                         .trackId = 1,
                         .rule = "person_in_alert_zone",
                         .severity = "critical",
                         .zoneKind = "alert", .signature = {}}),
      1)));
  CHECK(harness.notifications.calls == 1);
  CHECK(harness.camera.announceCalls == 2);
  CHECK(harness.camera.alarmCalls == 2);
  CHECK(journalField("nta:2", "suppression_reason") == "thread_suppressed");
  CHECK(journalField("nta:2", "did_notify") == "0");
}

TEST_CASE("an observation older than the stale window notifies but never speaks or sounds")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ThreadHarness harness;
  harness.config.announceLevel = 3;
  harness.config.alarmLevel = 4;
  harness.config.defaultMode = GuardMode::Armed;
  harness.config.staleObservationS = 120;
  auto service = harness.makeService();

  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "stale:1",
                         .cameraId = 37,
                         .trackId = 1,
                         .rule = "person_in_alert_zone",
                         .severity = "critical",
                         .zoneKind = "alert", .signature = {}}),
      2)));
  CHECK(harness.notifications.calls == 1);
  CHECK(harness.camera.announceCalls == 0);
  CHECK(harness.camera.alarmCalls == 0);
  CHECK(scalar("SELECT json_extract(checkpoint, '$.stale') FROM guard_observation_inbox "
               "WHERE event_id = 'stale:1'") == "1");
}

TEST_CASE("a first delivery that waited in a backlog is not stale and still deters")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ThreadHarness harness;
  harness.config.announceLevel = 3;
  harness.config.alarmLevel = 4;
  harness.config.defaultMode = GuardMode::Armed;
  harness.config.staleObservationS = 120;
  auto service = harness.makeService();

  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "stale:backlog",
                         .cameraId = 38,
                         .trackId = 1,
                         .rule = "person_in_alert_zone",
                         .severity = "critical",
                         .zoneKind = "alert", .signature = {}}),
      1)));
  CHECK(harness.notifications.calls == 1);
  CHECK(harness.camera.announceCalls + harness.camera.alarmCalls > 0);
  CHECK(scalar("SELECT json_extract(checkpoint, '$.stale') FROM guard_observation_inbox "
               "WHERE event_id = 'stale:backlog'") == "0");
}

TEST_CASE("closing an encounter journals without notifying")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ThreadHarness harness;
  auto service = harness.makeService();

  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "ntc:1",
                         .cameraId = 25,
                         .trackId = 1,
                         .rule = "person_in_alert_zone",
                         .severity = "critical",
                         .zoneKind = "alert", .signature = {}}),
      1)));
  CHECK(harness.notifications.calls == 1);
  const std::string encounterId = scalar(
      "SELECT id FROM guard_encounter WHERE best_camera_id = 25");
  REQUIRE_FALSE(encounterId.empty());
  DbService::client()->execSqlSync(
      "UPDATE guard_encounter SET last_seen = 1 WHERE id = " + encounterId);

  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  GuardRepository repository;
  REQUIRE(drogon::sync_wait(repository.closeStaleEncounters(
      {.olderThan = 1000, .closedAt = now, .decisionMode = "shadow"})));
  CHECK(harness.notifications.calls == 1);
  CHECK(scalar("SELECT COUNT(*) FROM guard_decision_journal WHERE event_id "
               "LIKE 'encounter:%' AND encounter_id = " +
               encounterId) == "1");
  CHECK(scalar("SELECT did_notify FROM guard_decision_journal WHERE event_id "
               "LIKE 'encounter:%' AND encounter_id = " +
               encounterId) == "0");
}

TEST_CASE("a crash between flip and thread record still converges")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ThreadHarness harness;
  *harness.failTarget = "between_flip_and_thread";
  auto service = harness.makeService();

  bool threw = false;
  try {
    drogon::sync_wait(service->handle(
        threadObservation({.eventId = "ntx:1",
                           .cameraId = 28,
                           .trackId = 1,
                           .rule = "person_in_alert_zone",
                           .severity = "critical",
                           .zoneKind = "alert", .signature = {}}),
        1));
  }
  catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
  CHECK(harness.notifications.calls == 1);
  CHECK(journalField("ntx:1", "did_notify") == "0");
  CHECK(threadField(28, "notify_count") == "0");

  *harness.failTarget = "";
  service = harness.makeService();
  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "ntx:1",
                         .cameraId = 28,
                         .trackId = 1,
                         .rule = "person_in_alert_zone",
                         .severity = "critical",
                         .zoneKind = "alert", .signature = {}}),
      2)));
  CHECK(harness.notifications.calls == 2);
  CHECK(journalField("ntx:1", "did_notify") == "1");
  CHECK(threadField(28, "notify_count") == "1");
  CHECK(threadField(28, "notify_highest_rank") == "4");

  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "ntx:2",
                         .cameraId = 28,
                         .trackId = 1,
                         .rule = "person_in_alert_zone",
                         .severity = "critical",
                         .zoneKind = "alert", .signature = {}}),
      1)));
  CHECK(harness.notifications.calls == 2);
  CHECK(journalField("ntx:2", "suppression_reason") == "thread_suppressed");
}

TEST_CASE("a lost settlement stays visible as ambiguous, never suppressed")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ThreadHarness harness;
  *harness.failTarget = "between_flip_and_thread";
  auto service = harness.makeService();

  for (int delivered = 1; delivered <= 3; ++delivered) {
    bool threw = false;
    try {
      drogon::sync_wait(service->handle(
          threadObservation({.eventId = "ntd:1",
                             .cameraId = 29,
                             .trackId = 1,
                             .rule = "person_in_alert_zone",
                             .severity = "critical",
                             .zoneKind = "alert", .signature = {}}),
          delivered));
    }
    catch (const std::exception&) {
      threw = true;
    }
    CHECK(threw);
  }
  CHECK(harness.notifications.calls == 3);

  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "ntd:1",
                         .cameraId = 29,
                         .trackId = 1,
                         .rule = "person_in_alert_zone",
                         .severity = "critical",
                         .zoneKind = "alert", .signature = {}}),
      4)));
  CHECK(scalar("SELECT status FROM guard_observation_inbox WHERE event_id = "
               "'ntd:1'") == "dead_lettered");
  CHECK(harness.notifications.calls == 3);
  CHECK(journalField("ntd:1", "did_notify") == "0");
  CHECK(scalar("SELECT dispatch_attempts FROM guard_decision_journal WHERE "
               "event_id = 'ntd:1'") == "3");
  CHECK(threadField(29, "notify_count") == "0");

  const int64_t journaledAt = std::stoll(scalar(
      "SELECT created_at FROM guard_decision_journal WHERE event_id = 'ntd:1'"));
  GuardRepository repository;
  const DecisionSummary summary = drogon::sync_wait(
      repository.summarizeDecisions({.from = journaledAt,
                                     .to = journaledAt,
                                     .nearMissMargin = 0}));
  CHECK(summary.ambiguousNotifications >= 1);

  *harness.failTarget = "";
  service = harness.makeService();
  const int callsBefore = harness.notifications.calls.load();
  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "ntd:2",
                         .cameraId = 29,
                         .trackId = 1,
                         .rule = "person_in_alert_zone",
                         .severity = "critical",
                         .zoneKind = "alert", .signature = {}}),
      1)));
  CHECK(harness.notifications.calls == callsBefore + 1);
}

TEST_CASE("notification reasons are the journal's reasons")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ThreadHarness harness;
  auto service = harness.makeService();

  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "phr:1",
                         .cameraId = 49,
                         .trackId = 1,
                         .rule = "person_in_alert_zone",
                         .severity = "critical",
                         .zoneKind = "alert",
                         .signature = {}}),
      1)));
  REQUIRE(harness.notifications.calls == 1);
  REQUIRE(harness.notifications.sent.size() == 1);
  const Json::Value data =
      json_util::fromString(harness.notifications.sent.front().data);
  const Json::Value journal =
      json_util::fromString(journalField("phr:1", "reasons"));
  REQUIRE(journal.isArray());
  CHECK(json_util::toString(data["reasons"]) == json_util::toString(journal));
  CHECK(harness.notifications.sent.front().body.find("strong detection") ==
        std::string::npos);
}

TEST_CASE("staging observations journal without notifying")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ThreadHarness harness;
  harness.config.stagingEnabled = true;
  auto service = harness.makeService();

  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "stg:1",
                         .cameraId = 40,
                         .trackId = 1,
                         .rule = "person_day",
                         .severity = "info",
                         .zoneKind = "monitor",
                         .signature = "stage-sig-1"}),
      1)));
  CHECK(harness.notifications.calls == 0);
  CHECK(journalField("stg:1", "suppression_reason") == "staging");
  CHECK(journalField("stg:1", "did_notify") == "0");
  CHECK(journalField("stg:1", "legacy_would_notify") == "1");
  CHECK(journalField("stg:1", "suppressed_kinds") == "[\"notify\"]");
  CHECK(journalField("stg:1", "repeat_visits") == "1");
  CHECK(std::stod(journalField("stg:1", "novelty_score")) == 1.0);
}

TEST_CASE("nightly darkness never floors to High")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ThreadHarness harness;
  auto service = harness.makeService();
  service->ingestHealth({.cameraId = 41, .status = "dark", .atMs = testNowMs()});

  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "tmp:1",
                         .cameraId = 41,
                         .trackId = 1,
                         .rule = "person_day",
                         .severity = "info",
                         .zoneKind = "monitor", .signature = {}}),
      1)));
  CHECK(harness.notifications.calls == 1);
  CHECK(journalField("tmp:1", "severity") == "medium");
  CHECK(journalField("tmp:1", "did_notify") == "1");

  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  const int64_t baseMs = testNowMs();
  for (int back = 300; back >= 0; back -= 60)
    service->ingestHealth({.cameraId = 41, .status = "dark", .atMs = baseMs - (back * int64_t{1000})});
  drogon::sync_wait(service->checkTamperSweep(now));
  CHECK(harness.notifications.calls == 1);
  CHECK(scalar("SELECT COUNT(*) FROM guard_decision_journal WHERE event_id "
               "LIKE 'tamper:41:%'") == "0");
}

TEST_CASE("a covered camera respects the staging hold")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ThreadHarness harness;
  harness.config.stagingEnabled = true;
  auto service = harness.makeService();
  service->ingestHealth({.cameraId = 42, .status = "covered", .atMs = testNowMs()});

  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "tmc:1",
                         .cameraId = 42,
                         .trackId = 1,
                         .rule = "person_day",
                         .severity = "info",
                         .zoneKind = "monitor", .signature = {}}),
      1)));
  CHECK(harness.notifications.calls == 0);
  CHECK(journalField("tmc:1", "severity") == "medium");
  CHECK(journalField("tmc:1", "suppression_reason") == "staging");
  CHECK(journalField("tmc:1", "did_notify") == "0");
}

TEST_CASE("a blurred camera respects the staging hold")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ThreadHarness harness;
  harness.config.stagingEnabled = true;
  auto service = harness.makeService();
  service->ingestHealth({.cameraId = 50, .status = "blurred", .atMs = testNowMs()});

  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "blh:1",
                         .cameraId = 50,
                         .trackId = 1,
                         .rule = "person_day",
                         .severity = "info",
                         .zoneKind = "monitor", .signature = {}}),
      1)));
  CHECK(harness.notifications.calls == 0);
  CHECK(journalField("blh:1", "severity") == "medium");
  CHECK(journalField("blh:1", "suppression_reason") == "staging");
  CHECK(journalField("blh:1", "did_notify") == "0");
}

TEST_CASE("a week-long blur pages exactly once")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ThreadHarness harness;
  CHECK(harness.config.tamperSustainedS == 300);
  CHECK(harness.config.healthStaleS == 300);
  auto service = harness.makeService();
  const int64_t weekStart = testNowMs() / 1000;
  for (int64_t hour = 0; hour <= 7 * 24; ++hour) {
    const int64_t at = (weekStart + hour * 3600) * 1000;
    service->ingestHealth({.cameraId = 150, .status = "blurred", .atMs = at});
    drogon::sync_wait(service->checkTamperSweep(weekStart + hour * 3600));
  }
  CHECK(harness.notifications.calls == 1);
  CHECK(scalar("SELECT COUNT(*) FROM guard_decision_journal WHERE event_id "
               "LIKE 'tamper:150:%'") == "1");
  CHECK(scalar("SELECT COUNT(*) FROM guard_decision_journal WHERE event_id "
               "LIKE 'tamper:150:%' AND did_notify = 1") == "1");
}

TEST_CASE("the alert re-arms after recovery and a new degradation")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ThreadHarness harness;
  auto service = harness.makeService();
  const int64_t start = testNowMs() / 1000;
  for (int64_t step = 0; step <= 300; step += 60) {
    service->ingestHealth({.cameraId = 151, .status = "blurred", .atMs = (start + step) * 1000});
    drogon::sync_wait(service->checkTamperSweep(start + step));
  }
  CHECK(harness.notifications.calls == 1);

  for (int64_t step = 360; step <= 900; step += 60) {
    service->ingestHealth({.cameraId = 151, .status = "ok", .atMs = (start + step) * 1000});
    drogon::sync_wait(service->checkTamperSweep(start + step));
  }
  CHECK(harness.notifications.calls == 1);
  CHECK(scalar("SELECT COUNT(*) FROM guard_state WHERE key LIKE "
               "'tamper_%_151'") == "0");

  for (int64_t step = 960; step <= 1260; step += 60) {
    service->ingestHealth({.cameraId = 151, .status = "blurred", .atMs = (start + step) * 1000});
    drogon::sync_wait(service->checkTamperSweep(start + step));
  }
  CHECK(harness.notifications.calls == 2);
  CHECK(scalar("SELECT COUNT(*) FROM guard_decision_journal WHERE event_id "
               "LIKE 'tamper:151:%' AND did_notify = 1") == "2");
}

TEST_CASE("a restart mid-window still fires once the window completes")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ThreadHarness harness;
  const int64_t wall = testNowMs() / 1000;
  {
    auto service = harness.makeService();
    for (int64_t step = 0; step <= 150; step += 30) {
      service->ingestHealth({.cameraId = 152, .status = "moved", .atMs = (wall + step) * 1000});
      drogon::sync_wait(service->checkTamperSweep(wall + step));
    }
    CHECK(harness.notifications.calls == 0);
    CHECK(scalar("SELECT value FROM guard_state WHERE key = "
                 "'tamper_onset_152'") == std::to_string(wall));
  }
  GuardService restarted({.bus = nullptr,
                          .identity = &harness.identity,
                          .notifications = &harness.notifications,
                          .actions = &harness.camera,
                          .assessment = nullptr},
                         harness.config);
  for (int64_t step = 151; step <= 301; step += 50) {
    restarted.ingestHealth({.cameraId = 152, .status = "moved", .atMs = (wall + step) * 1000});
    drogon::sync_wait(restarted.checkTamperSweep(wall + step));
  }
  CHECK(harness.notifications.calls == 1);
  CHECK(scalar("SELECT COUNT(*) FROM guard_decision_journal WHERE event_id = "
               "'tamper:152:" +
               std::to_string(wall) + "' AND did_notify = 1") == "1");
  drogon::sync_wait(restarted.checkTamperSweep(wall + 301));
  CHECK(harness.notifications.calls == 1);
}

TEST_CASE("an unknown health state is penalized but never escalates")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ThreadHarness harness;
  auto service = harness.makeService();
  service->ingestHealth({.cameraId = 144, .status = "insect", .atMs = testNowMs()});

  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "ins:1",
                         .cameraId = 144,
                         .trackId = 1,
                         .rule = "person_in_alert_zone",
                         .severity = "critical",
                         .zoneKind = "alert", .signature = {}}),
      1)));
  CHECK(harness.notifications.calls == 1);
  CHECK(journalField("ins:1", "did_notify") == "1");
  const std::string signals = journalField("ins:1", "belief_signals");
  CHECK(signals.find("camera_health_degraded") != std::string::npos);

  const int64_t base = testNowMs() / 1000;
  for (int64_t step = 0; step <= 300; step += 60) {
    service->ingestHealth({.cameraId = 144, .status = "insect", .atMs = (base + step) * 1000});
    drogon::sync_wait(service->checkTamperSweep(base + step));
  }
  CHECK(harness.notifications.calls == 1);
  CHECK(scalar("SELECT COUNT(*) FROM guard_decision_journal WHERE event_id "
               "LIKE 'tamper:144:%'") == "0");
}

TEST_CASE("quiet hours hold a medium alert for the morning summary")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ThreadHarness harness;
  harness.config.quietHoursEnabled = true;
  harness.config.quietStartHour = 0;
  harness.config.quietEndHour = 24;
  harness.config.quietDailyBudget = 1000;
  auto service = harness.makeService();

  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "qhm:1",
                         .cameraId = 45,
                         .trackId = 1,
                         .rule = "person_day",
                         .severity = "info",
                         .zoneKind = "monitor",
                         .signature = {}}),
      1)));
  CHECK(harness.notifications.calls == 0);
  CHECK(journalField("qhm:1", "did_notify") == "0");
  CHECK(journalField("qhm:1", "suppression_reason") == "held");
  CHECK(journalField("qhm:1", "quiet_hold") == "1");
  CHECK(journalField("qhm:1", "budget_hold") == "0");
  CHECK(scalar("SELECT detail FROM guard_action WHERE camera_id = 45 AND "
               "kind = 'notify' AND status = 'held'") == "quiet_hours");
}

TEST_CASE("quiet hours never hold a high alert")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ThreadHarness harness;
  harness.config.quietHoursEnabled = true;
  harness.config.quietStartHour = 0;
  harness.config.quietEndHour = 24;
  harness.config.quietDailyBudget = 1000;
  auto service = harness.makeService();

  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "qhh:1",
                         .cameraId = 245,
                         .trackId = 1,
                         .rule = "person_in_alert_zone",
                         .severity = "critical",
                         .zoneKind = "alert",
                         .signature = {}}),
      1)));
  CHECK(harness.notifications.calls == 1);
  CHECK(journalField("qhh:1", "did_notify") == "1");
  CHECK(journalField("qhh:1", "quiet_hold") == "0");
}

TEST_CASE("an exhausted daily budget holds the next medium alert")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ThreadHarness harness;
  harness.config.quietHoursEnabled = true;
  harness.config.quietStartHour = 0;
  harness.config.quietEndHour = 0;
  const std::time_t clock = std::time(nullptr);
  std::tm local{};
  localtime_r(&clock, &local);
  const int64_t midnight = static_cast<int64_t>(clock) -
                           (local.tm_hour * 3600 + local.tm_min * 60 +
                            local.tm_sec);
  harness.config.quietDailyBudget =
      std::stoi(scalar("SELECT COUNT(*) FROM guard_decision_journal WHERE "
                       "did_notify = 1 AND created_at >= " +
                       std::to_string(midnight))) +
      1;
  auto service = harness.makeService();

  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "bdg:1",
                         .cameraId = 46,
                         .trackId = 1,
                         .rule = "person_day",
                         .severity = "info",
                         .zoneKind = "monitor",
                         .signature = {}}),
      1)));
  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "bdg:2",
                         .cameraId = 47,
                         .trackId = 1,
                         .rule = "person_day",
                         .severity = "info",
                         .zoneKind = "monitor",
                         .signature = {}}),
      1)));
  CHECK(harness.notifications.calls == 1);
  CHECK(journalField("bdg:2", "did_notify") == "0");
  CHECK(journalField("bdg:2", "suppression_reason") == "held");
  CHECK(journalField("bdg:2", "quiet_hold") == "0");
  CHECK(journalField("bdg:2", "budget_hold") == "1");
}

TEST_CASE("repeat visits accumulate per signature")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ThreadHarness harness;
  auto service = harness.makeService();

  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "nrv:1",
                         .cameraId = 48,
                         .trackId = 1,
                         .rule = "person_in_alert_zone",
                         .severity = "critical",
                         .zoneKind = "alert",
                         .signature = "visits-sig-1"}),
      1)));
  CHECK(journalField("nrv:1", "repeat_visits") == "1");

  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "nrv:2",
                         .cameraId = 48,
                         .trackId = 1,
                         .rule = "person_in_alert_zone",
                         .severity = "critical",
                         .zoneKind = "alert",
                         .signature = "visits-sig-1"}),
      1)));
  CHECK(harness.notifications.calls == 1);
  CHECK(journalField("nrv:2", "suppression_reason") == "thread_suppressed");
  CHECK(journalField("nrv:2", "repeat_visits") == "2");
}

TEST_CASE("a failed tamper notify is retried, not recorded")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  FakeCameraActions camera;
  FakeIdentity identity;
  FlakyNotifications notifications;
  ThreadHarness harness;
  GuardService::Config quickRetry = harness.config;
  quickRetry.retryBaseMs = 1;
  quickRetry.retryMaxMs = 1;
  auto service = std::make_unique<GuardService>(
      GuardService::Dependencies{.bus = nullptr,
                                 .identity = &identity,
                                 .notifications = &notifications,
                                 .actions = &camera,
                                 .assessment = nullptr},
      quickRetry);

  const int64_t base = testNowMs() / 1000;
  for (int64_t step = 0; step <= 300; step += 60) {
    service->ingestHealth({.cameraId = 153, .status = "moved", .atMs = (base + step) * 1000});
    drogon::sync_wait(service->checkTamperSweep(base + step));
  }
  CHECK(notifications.calls == 1);
  CHECK(scalar("SELECT value FROM guard_state WHERE key = "
               "'tamper_notified_onset_153'") == "");
  CHECK(scalar("SELECT COUNT(*) FROM guard_decision_journal WHERE event_id = "
               "'tamper:153:" +
               std::to_string(base) + "' AND did_notify = 1") == "0");

  for (int64_t step = 360; step <= 660; step += 60) {
    service->ingestHealth({.cameraId = 153, .status = "moved", .atMs = (base + step) * 1000});
    drogon::sync_wait(service->checkTamperSweep(base + step));
  }
  CHECK(notifications.calls == 2);
  CHECK(scalar("SELECT COUNT(*) FROM guard_decision_journal WHERE event_id "
               "LIKE 'tamper:153:%' AND did_notify = 1") == "1");
  CHECK(scalar("SELECT value FROM guard_state WHERE key = "
               "'tamper_notified_onset_153'") != "");
}

TEST_CASE("a crash during the notification RPC still flags the row ambiguous")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  FakeCameraActions camera;
  FakeIdentity identity;
  ThrowingNotifications notifications;
  ThreadHarness harness;
  auto service = std::make_unique<GuardService>(
      GuardService::Dependencies{.bus = nullptr,
                                 .identity = &identity,
                                 .notifications = &notifications,
                                 .actions = &camera,
                                 .assessment = nullptr},
      harness.config);

  bool threw = false;
  try {
    drogon::sync_wait(service->handle(
        threadObservation({.eventId = "ntq:1",
                           .cameraId = 30,
                           .trackId = 1,
                           .rule = "person_in_alert_zone",
                           .severity = "critical",
                           .zoneKind = "alert", .signature = {}}),
        1));
  }
  catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
  CHECK(notifications.calls == 1);
  CHECK(journalField("ntq:1", "did_notify") == "0");
  CHECK(journalField("ntq:1", "legacy_would_notify") == "1");
  CHECK(scalar("SELECT dispatch_attempts FROM guard_decision_journal WHERE "
               "event_id = 'ntq:1'") == "1");
  CHECK(scalar("SELECT COUNT(*) FROM guard_decision_journal WHERE event_id = "
               "'ntq:1' AND did_notify = 0 AND dispatch_attempts > 0 AND "
               "legacy_would_notify = 1") == "1");
}

TEST_CASE("only effects that left count against the hourly caps")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  GuardRepository repository;
  const auto now = static_cast<int64_t>(std::time(nullptr));
  const auto record = [&](const std::string& id, const std::string& status) {
    drogon::sync_wait(repository.insertAction({.incidentId = 0,
                                               .encounterId = 0,
                                               .cameraId = 77,
                                               .personId = 0,
                                               .commandId = "cap:" + id,
                                               .kind = "notify",
                                               .status = status,
                                               .detail = {},
                                               .createdAt = now}));
  };
  record("1", "rejected");
  record("2", "budget_denied");
  record("3", "belief_suppressed");
  record("4", "thread_suppressed");
  record("5", "denied");
  CHECK(drogon::sync_wait(repository.effectsSince(77, now - 3600)) == 0);

  record("6", "succeeded");
  record("7", "in_flight");
  CHECK(drogon::sync_wait(repository.effectsSince(77, now - 3600)) == 2);
}

TEST_CASE("a hard floor notifies before the assessment and a weapon escalates it")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ThreadHarness harness;
  harness.config.announceLevel = 3;
  harness.config.alarmLevel = 4;
  ScriptedAssessment assessment(harness.notifications, {"knife"});
  harness.assessment = &assessment;
  auto service = harness.makeService();

  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "fast:1",
                         .cameraId = 52,
                         .trackId = 1,
                         .rule = "person_night",
                         .severity = "warning",
                         .zoneKind = "", .signature = {}}),
      1)));
  CHECK(assessment.calls == 1);
  CHECK(assessment.notificationsBefore == 1);
  CHECK(harness.notifications.calls == 2);
  CHECK(harness.camera.alarmCalls == 0);
  CHECK(harness.camera.announceCalls == 1);
  CHECK(scalar("SELECT danger FROM guard_incident WHERE event_id = 'fast:1'") ==
        "critical");
}

TEST_CASE("a soft case is still assessed before any effect")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ThreadHarness harness;
  ScriptedAssessment assessment(harness.notifications, {"visitor"});
  harness.assessment = &assessment;
  auto service = harness.makeService();

  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "soft:1",
                         .cameraId = 53,
                         .trackId = 1,
                         .rule = "person_day",
                         .severity = "info",
                         .zoneKind = "monitor", .signature = {}}),
      1)));
  CHECK(assessment.calls == 1);
  CHECK(assessment.notificationsBefore == 0);
}

TEST_CASE("a vetoed visit that lingers is never promoted")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ThreadHarness harness;
  harness.config.stagingEnabled = true;
  harness.config.loiterChecks = 2;
  ScriptedAssessment assessment(harness.notifications, {"carrying_box"}, true);
  harness.assessment = &assessment;
  auto service = harness.makeService();

  for (int check = 1; check <= 3; ++check) {
    REQUIRE(drogon::sync_wait(service->handle(
        threadObservation({.eventId = "veto:" + std::to_string(check),
                           .cameraId = 54,
                           .trackId = 1,
                           .rule = "person_day",
                           .severity = "info",
                           .zoneKind = "monitor", .signature = {}}),
        1)));
  }
  CHECK(assessment.calls == 3);
  CHECK(harness.notifications.calls == 0);
}

TEST_CASE("a higher tier inside the cooldown still acts")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ThreadHarness harness;
  harness.config.actionCooldownS = 600;
  auto service = harness.makeService();

  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "tier:1",
                         .cameraId = 55,
                         .trackId = 1,
                         .rule = "person_day",
                         .severity = "info",
                         .zoneKind = "monitor", .signature = {}}),
      1)));
  CHECK(harness.notifications.calls == 1);

  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "tier:2",
                         .cameraId = 55,
                         .trackId = 1,
                         .rule = "person_day",
                         .severity = "info",
                         .zoneKind = "monitor", .signature = {}}),
      1)));
  CHECK(harness.notifications.calls == 1);

  REQUIRE(drogon::sync_wait(service->handle(
      threadObservation({.eventId = "tier:3",
                         .cameraId = 55,
                         .trackId = 1,
                         .rule = "person_in_alert_zone",
                         .severity = "critical",
                         .zoneKind = "alert", .signature = {}}),
      1)));
  CHECK(harness.notifications.calls == 2);
}

TEST_CASE("the history purge removes only settled rows past the window")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  auto client = DbService::client();
  client->execSqlSync(
      "INSERT INTO guard_incident (camera_id, event_id, created_at) VALUES "
      "(90, 'purge:old', 100), (90, 'purge:new', 9000000000)");
  client->execSqlSync(
      "INSERT INTO guard_observation_inbox (event_id, camera_id, status, "
      "updated_at, completed_at) VALUES ('purge:done', 90, 'completed', 100, "
      "100), ('purge:live', 90, 'processing', 100, 0)");
  client->execSqlSync(
      "INSERT INTO guard_action_outbox (command_id, camera_id, kind, status, "
      "created_at, updated_at) VALUES ('purge:sent', 90, 'notify', "
      "'succeeded', 100, 100), ('purge:retry', 90, 'notify', "
      "'retryable_failed', 100, 100)");
  client->execSqlSync(
      "INSERT INTO guard_encounter (person_id, state, first_seen, last_seen) "
      "VALUES (0, 'closed', 100, 100), (0, 'observing', 100, 100)");

  GuardRepository repository;
  const int64_t removed = drogon::sync_wait(
      repository.purgeHistory({.historyBefore = 1000, .inboxBefore = 1000}));
  CHECK(removed >= 4);
  CHECK(scalar("SELECT COUNT(*) FROM guard_incident WHERE event_id LIKE "
               "'purge:%'") == "1");
  CHECK(scalar("SELECT event_id FROM guard_observation_inbox WHERE event_id "
               "LIKE 'purge:%'") == "purge:live");
  CHECK(scalar("SELECT command_id FROM guard_action_outbox WHERE command_id "
               "LIKE 'purge:%'") == "purge:retry");
  CHECK(scalar("SELECT COUNT(*) FROM guard_encounter WHERE last_seen = 100 "
               "AND state = 'observing'") == "1");
  CHECK(scalar("SELECT COUNT(*) FROM guard_encounter WHERE last_seen = 100 "
               "AND state = 'closed'") == "0");
}

TEST_CASE("a camera offline past the offline window is one critical tamper notice while away")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ThreadHarness harness;
  harness.config.defaultMode = GuardMode::Away;
  auto service = harness.makeService();

  const int64_t base = testNowMs() / 1000;
  for (int64_t step = 0; step < 240; step += 60) {
    service->ingestHealth({.cameraId = 171, .status = "unreachable", .atMs = (base + step) * 1000});
    drogon::sync_wait(service->checkTamperSweep(base + step));
  }
  CHECK(harness.notifications.calls == 0);
  for (int64_t step = 240; step <= 360; step += 60) {
    service->ingestHealth({.cameraId = 171, .status = "unreachable", .atMs = (base + step) * 1000});
    drogon::sync_wait(service->checkTamperSweep(base + step));
  }
  REQUIRE(harness.notifications.calls == 1);
  const Json::Value data = json_util::fromString(harness.notifications.sent.front().data);
  CHECK(data["kind"].asString() == "guard_tamper");
  CHECK(data["urgency"].asString() == "critical");
  CHECK(data["danger"].asString() == "critical");
  CHECK(data["threadKey"].asString() == "guard:tamper:171");
  CHECK(harness.notifications.sent.front().body.find("desenchufada") != std::string::npos);
  CHECK(scalar("SELECT danger FROM guard_incident WHERE event_id = 'tamper:171:" +
               std::to_string(base) + "'") == "critical");
}

TEST_CASE("a short offline blip stays quiet, and offline at home is not critical")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ThreadHarness harness;
  auto service = harness.makeService();

  const int64_t base = testNowMs() / 1000;
  for (int64_t step = 0; step <= 180; step += 60) {
    service->ingestHealth({.cameraId = 172, .status = "unreachable", .atMs = (base + step) * 1000});
    drogon::sync_wait(service->checkTamperSweep(base + step));
  }
  for (int64_t step = 240; step <= 600; step += 60) {
    service->ingestHealth({.cameraId = 172, .status = "ok", .atMs = (base + step) * 1000});
    drogon::sync_wait(service->checkTamperSweep(base + step));
  }
  CHECK(harness.notifications.calls == 0);

  for (int64_t step = 660; step <= 900; step += 60) {
    service->ingestHealth({.cameraId = 172, .status = "unreachable", .atMs = (base + step) * 1000});
    drogon::sync_wait(service->checkTamperSweep(base + step));
  }
  REQUIRE(harness.notifications.calls == 1);
  const Json::Value data = json_util::fromString(harness.notifications.sent.front().data);
  CHECK(data["urgency"].asString() == "active");
  CHECK(data["danger"].asString() == "high");
}

TEST_CASE("an expected guest in an alert zone is never vetoed below the guest cap")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  scalar("UPDATE guard_environment SET mode = 'home'");
  scalar("DELETE FROM guard_expected_guest");
  const int64_t nowS = testNowMs() / 1000;
  scalar("INSERT INTO guard_expected_guest (description, camera_id, valid_from, "
         "valid_until) VALUES ('plumber', 0, " +
         std::to_string(nowS - 600) + ", " + std::to_string(nowS + 3600) + ")");
  const auto run = [](const std::string& eventId, int64_t cameraId,
                      ScriptedAssessment& assessment, ThreadHarness& harness) {
    harness.assessment = &assessment;
    auto service = harness.makeService();
    REQUIRE(drogon::sync_wait(service->handle(
        threadObservation({.eventId = eventId,
                           .cameraId = cameraId,
                           .trackId = 1,
                           .rule = "person_in_alert_zone",
                           .severity = "critical",
                           .zoneKind = "alert", .signature = {}}),
        1)));
    return journalField(eventId, "severity");
  };

  ThreadHarness vetoed;
  ScriptedAssessment calm(vetoed.notifications, {"carrying_box"}, true, "none");
  CHECK(run("guest:veto", 91, calm, vetoed) == "medium");
  CHECK(calm.calls == 1);

  ThreadHarness armed;
  ScriptedAssessment knife(armed.notifications, {"weapon"}, true, "none");
  CHECK(run("guest:knife", 92, knife, armed) == "critical");

  ThreadHarness raised;
  ScriptedAssessment object(raised.notifications, {"raised_object"}, false, "medium");
  CHECK(run("guest:raised", 93, object, raised) == "critical");

  scalar("DELETE FROM guard_expected_guest");
}

TEST_CASE("queued and running deliveries are kept in progress until they settle")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ThreadHarness harness;
  GatedAssessment assessment;
  harness.assessment = &assessment;
  auto service = harness.makeService();
  std::atomic<int> acked{0};
  std::atomic<int> runningTouches{0};
  std::atomic<int> queuedTouches{0};
  const auto delivery = [&acked](const std::string& eventId, std::atomic<int>& touches) {
    return GuardService::Delivery{
        .payload = json_util::toString(threadObservation({.eventId = eventId,
                                                          .cameraId = 61,
                                                          .trackId = 1,
                                                          .rule = "person_day",
                                                          .severity = "info",
                                                          .zoneKind = "monitor",
                                                          .signature = {}})),
        .ack = [&acked]() { acked.fetch_add(1); },
        .nak = {},
        .term = {},
        .inProgress = [&touches]() { touches.fetch_add(1); },
        .delivered = 1};
  };
  service->accept(delivery("ka:1", runningTouches));
  service->accept(delivery("ka:2", queuedTouches));
  for (int attempt = 0; attempt < 500 && assessment.entered.load() == 0; ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  REQUIRE(assessment.entered.load() == 1);
  CHECK(service->keepDeliveriesAlive() == 2);
  CHECK(runningTouches.load() == 1);
  CHECK(queuedTouches.load() == 1);
  assessment.open();
  for (int attempt = 0; attempt < 1000 && acked.load() < 2; ++attempt)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  REQUIRE(acked.load() == 2);
  CHECK(service->keepDeliveriesAlive() == 0);
  CHECK(runningTouches.load() == 1);
  CHECK(queuedTouches.load() == 1);
}
