#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <atomic>
#include <camera/camera-action-client.hxx>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <doctest/doctest.h>
#include <drogon/drogon.h>
#include <functional>
#include <feature/api/guard/services/guard-feature-service.hxx>
#include <guard-repository.hxx>
#include <guard-schema.hxx>
#include <guard-service.hxx>
#include <identity/identity-client.hxx>
#include <memory>
#include <notification/notification-client.hxx>
#include <optional>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/utils/json-util/json-util.hxx>
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
      const argus::sdk::CallerIdentity&) const override
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

class ThrowingNotifications final : public NotificationClient
{
public:
  ThrowingNotifications()
      : NotificationClient({.target = "127.0.0.1:1", .credential = {}})
  {
  }

  NotificationCreateResult createNotifications(
      const argus::notification::v1::CreateNotificationsRequest&,
      const argus::sdk::CallerIdentity&) const override
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
                                   .assessment = nullptr},
        serviceConfig);
  }
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
} // namespace

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
        "front: person_in_alert_zone");
  CHECK(harness.notifications.sent.front().body ==
        "Unrecognized person in the alert zone for 18s (strong detection, "
        "lingering, steady track)");
  const Json::Value data =
      json_util::fromString(harness.notifications.sent.front().data);
  CHECK(data["zoneKind"].asString() == "alert");
  CHECK(data["dwellS"].asInt64() == 18);
  CHECK(data["identityState"].asString() == "unrecognized");
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

  // Leased redelivery, the same path the retry pump drives: it converts a
  // scheduled retry deterministically instead of racing the retry clock.
  // The backoff above is expired by hand so no wall-clock wait is needed.
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
  const int64_t windowStart = static_cast<int64_t>(std::time(nullptr));

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
  const int64_t windowEnd = static_cast<int64_t>(std::time(nullptr));

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

  GuardRepository repository;
  const DecisionSummary summary = drogon::sync_wait(
      repository.summarizeDecisions({.from = windowStart,
                                     .to = windowEnd,
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

TEST_CASE("notification body and journal summary share one phrase set")
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
  const std::string body = harness.notifications.sent.front().body;
  GuardFeatureService feature(nullptr);
  const Json::Value decisions = drogon::sync_wait(feature.decisions(
      {.limit = 200,
       .from = 0,
       .to = 0,
       .cameraId = 49,
       .severity = {},
       .decisionMode = {},
       .suppressionReason = {},
       .divergentOnly = false,
       .nearMissMargin = 0,
       .afterCreatedAt = 0,
       .afterEventId = {}}));
  REQUIRE(decisions["rows"].size() == 1);
  const std::string summary =
      decisions["rows"][0]["summary"].asString();
  REQUIRE_FALSE(summary.empty());
  const auto paren = [](const std::string& text) {
    const auto open = text.find('(');
    const auto close = text.find(')', open);
    if (open == std::string::npos || close == std::string::npos)
      return std::string{};
    return text.substr(open, close - open + 1);
  };
  CHECK(paren(body) == "(strong detection, lingering, steady track)");
  CHECK(paren(summary) == paren(body));
  CHECK(body.find("nrecognized person (unrecognized") == std::string::npos);
  CHECK(body.find("nidentified person (unidentified") == std::string::npos);
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
  service->ingestHealth(41, "dark", testNowMs());

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
    service->ingestHealth(41, "dark", baseMs - back * 1000);
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
  service->ingestHealth(42, "covered", testNowMs());

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
  service->ingestHealth(50, "blurred", testNowMs());

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
    service->ingestHealth(150, "blurred", at);
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
    service->ingestHealth(151, "blurred", (start + step) * 1000);
    drogon::sync_wait(service->checkTamperSweep(start + step));
  }
  CHECK(harness.notifications.calls == 1);

  for (int64_t step = 360; step <= 900; step += 60) {
    service->ingestHealth(151, "ok", (start + step) * 1000);
    drogon::sync_wait(service->checkTamperSweep(start + step));
  }
  CHECK(harness.notifications.calls == 1);
  CHECK(scalar("SELECT COUNT(*) FROM guard_state WHERE key LIKE "
               "'tamper_%_151'") == "0");

  for (int64_t step = 960; step <= 1260; step += 60) {
    service->ingestHealth(151, "blurred", (start + step) * 1000);
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
      service->ingestHealth(152, "moved", (wall + step) * 1000);
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
  // Sim-time continuation past the restart: the 150 s earned before it count.
  for (int64_t step = 151; step <= 301; step += 50) {
    restarted.ingestHealth(152, "moved", (wall + step) * 1000);
    drogon::sync_wait(restarted.checkTamperSweep(wall + step));
  }
  CHECK(harness.notifications.calls == 1);
  CHECK(scalar("SELECT COUNT(*) FROM guard_decision_journal WHERE event_id = "
               "'tamper:152:" +
               std::to_string(wall + 301) + "' AND did_notify = 1") == "1");
  drogon::sync_wait(restarted.checkTamperSweep(wall + 301));
  CHECK(harness.notifications.calls == 1);
}

TEST_CASE("an unknown health state is penalized but never escalates")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ThreadHarness harness;
  auto service = harness.makeService();
  service->ingestHealth(144, "insect", testNowMs());

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
    service->ingestHealth(144, "insect", (base + step) * 1000);
    drogon::sync_wait(service->checkTamperSweep(base + step));
  }
  CHECK(harness.notifications.calls == 1);
  CHECK(scalar("SELECT COUNT(*) FROM guard_decision_journal WHERE event_id "
               "LIKE 'tamper:144:%'") == "0");
}

TEST_CASE("quiet-hours marks without silencing")
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
  CHECK(harness.notifications.calls == 1);
  CHECK(journalField("qhm:1", "did_notify") == "1");
  CHECK(journalField("qhm:1", "quiet_hold") == "1");
  CHECK(journalField("qhm:1", "budget_hold") == "0");
}

TEST_CASE("an exhausted budget marks without silencing")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  ThreadHarness harness;
  harness.config.quietHoursEnabled = true;
  harness.config.quietStartHour = 0;
  harness.config.quietEndHour = 0;
  harness.config.quietDailyBudget = 1;
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
  CHECK(harness.notifications.calls == 2);
  CHECK(journalField("bdg:2", "did_notify") == "1");
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
  auto service = std::make_unique<GuardService>(
      GuardService::Dependencies{.bus = nullptr,
                                 .identity = &identity,
                                 .notifications = &notifications,
                                 .actions = &camera,
                                 .assessment = nullptr},
      harness.config);

  const int64_t base = testNowMs() / 1000;
  for (int64_t step = 0; step <= 300; step += 60) {
    service->ingestHealth(153, "moved", (base + step) * 1000);
    drogon::sync_wait(service->checkTamperSweep(base + step));
  }
  CHECK(notifications.calls == 1);
  CHECK(scalar("SELECT value FROM guard_state WHERE key = "
               "'tamper_notified_onset_153'") == "");
  CHECK(scalar("SELECT COUNT(*) FROM guard_decision_journal WHERE event_id = "
               "'tamper:153:" +
               std::to_string(base + 300) + "' AND did_notify = 1") == "0");

  for (int64_t step = 360; step <= 660; step += 60) {
    service->ingestHealth(153, "moved", (base + step) * 1000);
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
