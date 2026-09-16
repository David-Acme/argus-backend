#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <atomic>
#include <camera/camera-action-client.hxx>
#include <chrono>
#include <cstdio>
#include <doctest/doctest.h>
#include <drogon/drogon.h>
#include <functional>
#include <guard-repository.hxx>
#include <guard-schema.hxx>
#include <guard-service.hxx>
#include <identity/identity-client.hxx>
#include <memory>
#include <notification/notification-client.hxx>
#include <optional>
#include <shared/enums.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <feature/api/guard/services/guard-feature-service.hxx>
#include <feature/api/guard/dtos/feedback-decision-dto.hxx>
#include <feature/api/guard/dtos/summary-decisions-dto.hxx>
#include <shared/validation/validation_dsl.hxx>
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
  TempDb db{"guard-decision-journal-test"};
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

struct AlertObservationInput
{
  std::string eventId;
  int64_t cameraId{0};
  int64_t trackId{0};
};

Json::Value alertObservation(const AlertObservationInput& input)
{
  Json::Value event(Json::objectValue);
  event["schemaVersion"] = 2;
  event["eventId"] = input.eventId;
  event["cameraId"] = Json::Int64(input.cameraId);
  event["cameraName"] = "front";
  event["rule"] = "person_in_alert_zone";
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

GuardService::Config journalConfig()
{
  GuardService::Config config;
  config.enabled = true;
  config.profile = "home";
  config.defaultMode = GuardMode::Home;
  config.greetEnabled = false;
  config.greetReplyEnabled = false;
  config.announceLevel = 9;
  config.alarmLevel = 9;
  config.encounterTimeoutS = 300;
  config.crossCameraWindowS = 60;
  config.continuityWindowS = 60;
  config.loiterChecks = 3;
  config.stagingEnabled = false;
  config.maxDialogueTurns = 3;
  return config;
}

struct JournalHarness
{
  FakeCameraActions camera;
  FakeIdentity identity;
  CountingNotifications notifications;
  std::shared_ptr<std::string> failTarget =
      std::make_shared<std::string>();
  GuardService::Config config = journalConfig();

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

std::string journalField(const std::string& eventId, const std::string& column)
{
  return scalar("SELECT " + column + " FROM guard_decision_journal WHERE "
                "event_id = '" +
                eventId + "'");
}
} // namespace

TEST_CASE("shadow mode journals the verdict without changing behavior")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  JournalHarness harness;
  harness.config.decisionMode = "shadow";
  auto service = harness.makeService();

  REQUIRE(drogon::sync_wait(service->handle(
      alertObservation(
          {.eventId = "djs:1", .cameraId = 11, .trackId = 1}),
      1)));
  CHECK(scalar("SELECT status FROM guard_observation_inbox WHERE event_id = "
               "'djs:1'") == "completed");
  CHECK(harness.notifications.calls == 1);
  CHECK(scalar("SELECT COUNT(*) FROM guard_decision_journal WHERE event_id = "
               "'djs:1'") == "1");
  CHECK(journalField("djs:1", "decision_mode") == "shadow");
  CHECK(journalField("djs:1", "legacy_would_notify") == "1");
  CHECK(journalField("djs:1", "belief_would_notify") == "0");
  CHECK(journalField("djs:1", "did_notify") == "1");
  CHECK(journalField("djs:1", "suppression_reason") == "none");
  CHECK(journalField("djs:1", "belief_score") == "-4");
}

TEST_CASE("redelivery leaves exactly one journal row")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  JournalHarness harness;
  harness.config.decisionMode = "shadow";
  auto service = harness.makeService();

  REQUIRE(drogon::sync_wait(service->handle(
      alertObservation(
          {.eventId = "djr:1", .cameraId = 12, .trackId = 1}),
      1)));
  REQUIRE(drogon::sync_wait(service->handle(
      alertObservation(
          {.eventId = "djr:1", .cameraId = 12, .trackId = 1}),
      2)));
  CHECK(scalar("SELECT COUNT(*) FROM guard_decision_journal WHERE event_id = "
               "'djr:1'") == "1");
  CHECK(journalField("djr:1", "did_notify") == "1");
}

TEST_CASE("redelivery collects the calibration signals once")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  JournalHarness harness;
  harness.config.decisionMode = "shadow";
  auto service = harness.makeService();

  const auto baselineOf = [](int64_t cameraId) {
    const auto rows = DbService::client()->execSqlSync(
        "SELECT events_ema FROM guard_hourly_baseline WHERE camera_id = ?",
        cameraId);
    return rows.empty() ? 0.0 : rows.front()["events_ema"].as<double>();
  };

  *harness.failTarget = "before_effect:notify";
  bool threw = false;
  try {
    drogon::sync_wait(service->handle(
        alertObservation({.eventId = "djb:1", .cameraId = 32, .trackId = 1}),
        1));
  }
  catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
  CHECK(scalar("SELECT COUNT(*) FROM guard_decision_journal WHERE event_id = "
               "'djb:1'") == "1");
  CHECK(scalar("SELECT COUNT(*) FROM guard_hourly_baseline WHERE camera_id = "
               "32") == "1");
  const double afterFirst = baselineOf(32);
  CHECK(afterFirst > 0.0);

  *harness.failTarget = "";
  service = harness.makeService();
  REQUIRE(drogon::sync_wait(service->handle(
      alertObservation(
          {.eventId = "djb:1", .cameraId = 32, .trackId = 1}),
      2)));
  CHECK(baselineOf(32) == afterFirst);
}

TEST_CASE("enforce mode suppresses below-threshold effects")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  JournalHarness harness;
  harness.config.decisionMode = "enforce";
  auto service = harness.makeService();

  REQUIRE(drogon::sync_wait(service->handle(
      alertObservation(
          {.eventId = "dje:1", .cameraId = 13, .trackId = 1}),
      1)));
  CHECK(scalar("SELECT status FROM guard_observation_inbox WHERE event_id = "
               "'dje:1'") == "completed");
  CHECK(harness.notifications.calls == 0);
  CHECK(journalField("dje:1", "suppression_reason") == "belief_gate");
  CHECK(journalField("dje:1", "did_notify") == "0");
  CHECK(scalar("SELECT COUNT(*) FROM guard_action WHERE kind = 'notify' AND "
               "status = 'belief_suppressed' AND incident_id IN (SELECT id "
               "FROM guard_incident WHERE event_id = 'dje:1')") == "1");
}

TEST_CASE("a journal write failure does not fail the saga")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  JournalHarness harness;
  harness.config.decisionMode = "shadow";
  *harness.failTarget = "journal_write";
  auto service = harness.makeService();

  REQUIRE(drogon::sync_wait(service->handle(
      alertObservation(
          {.eventId = "djf:1", .cameraId = 14, .trackId = 1}),
      1)));
  CHECK(scalar("SELECT status FROM guard_observation_inbox WHERE event_id = "
               "'djf:1'") == "completed");
  CHECK(harness.notifications.calls == 1);
  CHECK(scalar("SELECT COUNT(*) FROM guard_decision_journal WHERE event_id = "
               "'djf:1'") == "0");
}

TEST_CASE("an unknown suppression reason fails closed")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  CHECK_FALSE(decisionSuppressionFromString("bogus").has_value());

  GuardRepository repository;
  REQUIRE(drogon::sync_wait(repository.insertDecisionJournal(
      {.eventId = "djx:closed",
       .encounterId = 1,
       .incidentId = 1,
       .cameraId = 16,
       .observationId = "16:1:100",
       .severity = "medium",
       .severityRank = 2,
       .hardFloor = false,
       .beliefScore = 5,
       .beliefSignals = "[]",
       .beliefThreshold = 5,
       .legacyWouldNotify = true,
       .beliefWouldNotify = true,
       .didNotify = true,
       .decisionMode = "shadow",
       .suppression = DecisionSuppression::None,
       .suppressedKinds = "[]",
       .noveltyScore = 0.0,
       .repeatVisits = 0,
       .quietHold = false,
       .budgetHold = false,
       .assessMs = 0,
       .createdAt = 1700000001})));

  bool rejected = false;
  try {
    DbService::client()->execSqlSync(
        "INSERT INTO guard_decision_journal (event_id, suppression_reason, "
        "created_at) VALUES ('djx:bogus', 'bogus', 1)");
  }
  catch (const std::exception&) {
    rejected = true;
  }
  CHECK(rejected);

  const auto rows = drogon::sync_wait(repository.listDecisions(200));
  bool found = false;
  for (const auto& row : rows) {
    if (row.eventId != "djx:closed")
      continue;
    found = true;
    CHECK(row.suppressionReason == "none");
    CHECK(row.didNotify);
  }
  CHECK(found);
}

TEST_CASE("the decisions endpoint serves the journal rows")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  GuardRepository repository;
  REQUIRE(drogon::sync_wait(repository.insertDecisionJournal(
      {.eventId = "djx:1",
       .encounterId = 1,
       .incidentId = 1,
       .cameraId = 15,
       .observationId = "15:1:100",
       .severity = "critical",
       .severityRank = 4,
       .hardFloor = true,
       .beliefScore = -4,
       .beliefSignals = "[\"identity_unavailable\"]",
       .beliefThreshold = 1,
       .legacyWouldNotify = true,
       .beliefWouldNotify = false,
       .didNotify = false,
       .decisionMode = "shadow",
       .suppression = DecisionSuppression::None,
       .suppressedKinds = "[]",
       .noveltyScore = 0.0,
       .repeatVisits = 0,
       .quietHold = false,
       .budgetHold = false,
       .assessMs = 0,
       .createdAt = 1700000000})));

  GuardFeatureService feature(nullptr);
  const Json::Value decisions =
      drogon::sync_wait(feature.decisions({.limit = 200,
                                           .from = 0,
                                           .to = 0,
                                           .cameraId = 0,
                                           .severity = {},
                                           .decisionMode = {},
                                           .suppressionReason = {},
                                           .divergentOnly = false,
                                           .nearMissMargin = 0,
                                           .afterCreatedAt = 0,
                                           .afterEventId = {}}));
  REQUIRE(decisions.isObject());
  REQUIRE(decisions["rows"].isArray());
  CHECK_FALSE(decisions["hasMore"].asBool());
  CHECK(decisions["nextCursor"].isNull());
  bool found = false;
  for (const auto& entry : decisions["rows"]) {
    if (entry["eventId"].asString() != "djx:1")
      continue;
    found = true;
    CHECK(entry["suppressionReason"].asString() == "none");
    CHECK_FALSE(entry["didNotify"].asBool());
    CHECK(entry["legacyWouldNotify"].asBool());
    CHECK_FALSE(entry["beliefWouldNotify"].asBool());
    CHECK(entry["beliefScore"].asInt() == -4);
    CHECK(entry["suppressedKinds"].isArray());
    CHECK(entry["suppressedKinds"].size() == 0);
  }
  CHECK(found);
}

TEST_CASE("decisions filters narrow rows and paginate with a stable cursor")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  GuardRepository repository;
  struct PageRowInput
  {
    std::string eventId;
    int64_t createdAt{0};
    std::string reason;
    bool legacy{false};
    bool belief{false};
  };
  const auto insertRow = [&](const PageRowInput& row) {
    return drogon::sync_wait(repository.insertDecisionJournal(
        {.eventId = row.eventId,
         .encounterId = 1,
         .incidentId = 1,
         .cameraId = 91,
         .observationId = "91:1:100",
         .severity = "high",
         .severityRank = 3,
         .hardFloor = false,
         .beliefScore = 4,
         .beliefSignals = "[]",
         .beliefThreshold = 3,
         .legacyWouldNotify = row.legacy,
         .beliefWouldNotify = row.belief,
         .didNotify = false,
         .decisionMode = "shadow",
         .suppression = decisionSuppressionFromString(row.reason).value(),
         .suppressedKinds = "[]",
         .noveltyScore = 0.0,
         .repeatVisits = 0,
         .quietHold = false,
         .budgetHold = false,
         .assessMs = 0,
         .createdAt = row.createdAt}));
  };
  REQUIRE(insertRow({.eventId = "pg:1",
                     .createdAt = 1800000100,
                     .reason = "none",
                     .legacy = true,
                     .belief = true}));
  REQUIRE(insertRow({.eventId = "pg:2",
                     .createdAt = 1800000100,
                     .reason = "thread_suppressed",
                     .legacy = true,
                     .belief = true}));
  REQUIRE(insertRow({.eventId = "pg:3",
                     .createdAt = 1800000099,
                     .reason = "none",
                     .legacy = true,
                     .belief = false}));

  GuardFeatureService feature(nullptr);
  const auto filtered = drogon::sync_wait(feature.decisions(
      {.limit = 200,
       .from = 0,
       .to = 0,
       .cameraId = 91,
       .severity = "high",
       .decisionMode = {},
       .suppressionReason = {},
       .divergentOnly = false,
       .nearMissMargin = 0,
       .afterCreatedAt = 0,
       .afterEventId = {}}));
  CHECK(filtered["rows"].size() == 3);

  const auto divergent = drogon::sync_wait(feature.decisions(
      {.limit = 200,
       .from = 0,
       .to = 0,
       .cameraId = 91,
       .severity = {},
       .decisionMode = {},
       .suppressionReason = {},
       .divergentOnly = true,
       .nearMissMargin = 0,
       .afterCreatedAt = 0,
       .afterEventId = {}}));
  REQUIRE(divergent["rows"].size() == 1);
  CHECK(divergent["rows"][0]["eventId"].asString() == "pg:3");

  const auto reasoned = drogon::sync_wait(feature.decisions(
      {.limit = 200,
       .from = 0,
       .to = 0,
       .cameraId = 91,
       .severity = {},
       .decisionMode = {},
       .suppressionReason = "thread_suppressed",
       .divergentOnly = false,
       .nearMissMargin = 0,
       .afterCreatedAt = 0,
       .afterEventId = {}}));
  REQUIRE(reasoned["rows"].size() == 1);
  CHECK(reasoned["rows"][0]["eventId"].asString() == "pg:2");

  const auto ranged = drogon::sync_wait(feature.decisions(
      {.limit = 200,
       .from = 1800000100,
       .to = 0,
       .cameraId = 91,
       .severity = {},
       .decisionMode = {},
       .suppressionReason = {},
       .divergentOnly = false,
       .nearMissMargin = 0,
       .afterCreatedAt = 0,
       .afterEventId = {}}));
  CHECK(ranged["rows"].size() == 2);

  const auto first = drogon::sync_wait(feature.decisions(
      {.limit = 2,
       .from = 0,
       .to = 0,
       .cameraId = 91,
       .severity = {},
       .decisionMode = {},
       .suppressionReason = {},
       .divergentOnly = false,
       .nearMissMargin = 0,
       .afterCreatedAt = 0,
       .afterEventId = {}}));
  REQUIRE(first["rows"].size() == 2);
  CHECK(first["rows"][0]["eventId"].asString() == "pg:2");
  CHECK(first["rows"][1]["eventId"].asString() == "pg:1");
  CHECK(first["hasMore"].asBool());
  REQUIRE(first["nextCursor"].isObject());
  const int64_t cursorAt = first["nextCursor"]["createdAt"].asInt64();
  const std::string cursorId =
      first["nextCursor"]["eventId"].asString();
  CHECK(cursorAt == 1800000100);
  CHECK(cursorId == "pg:1");

  const auto second = drogon::sync_wait(feature.decisions(
      {.limit = 2,
       .from = 0,
       .to = 0,
       .cameraId = 91,
       .severity = {},
       .decisionMode = {},
       .suppressionReason = {},
       .divergentOnly = false,
       .nearMissMargin = 0,
       .afterCreatedAt = cursorAt,
       .afterEventId = cursorId}));
  REQUIRE(second["rows"].size() == 1);
  CHECK(second["rows"][0]["eventId"].asString() == "pg:3");
  CHECK_FALSE(second["hasMore"].asBool());
  CHECK(second["nextCursor"].isNull());
}

TEST_CASE("decisions summary aggregates in SQL over a bounded window")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  GuardRepository repository;
  struct SummaryRowInput
  {
    std::string eventId;
    int score{0};
    bool fired{false};
    std::string mode;
    std::string signals;
  };
  const auto insertRow = [&](const SummaryRowInput& row) {
    return drogon::sync_wait(repository.insertDecisionJournal(
        {.eventId = row.eventId,
         .encounterId = 1,
         .incidentId = 1,
         .cameraId = 90,
         .observationId = "90:1:100",
         .severity = "high",
         .severityRank = 3,
         .hardFloor = false,
         .beliefScore = row.score,
         .beliefSignals = row.signals,
         .beliefThreshold = 5,
         .legacyWouldNotify = true,
         .beliefWouldNotify = true,
         .didNotify = row.fired,
         .decisionMode = row.mode,
         .suppression = DecisionSuppression::None,
         .suppressedKinds = "[]",
         .noveltyScore = 0.0,
         .repeatVisits = 0,
         .quietHold = false,
         .budgetHold = false,
         .assessMs = 0,
         .createdAt = 1800000000}));
  };
  REQUIRE(insertRow({.eventId = "sm:1",
                     .score = 3,
                     .fired = true,
                     .mode = "shadow",
                     .signals = "[\"detector_strong\"]"}));
  REQUIRE(insertRow({.eventId = "sm:2",
                     .score = 6,
                     .fired = false,
                     .mode = "enforce",
                     .signals = "[\"detector_strong\",\"persistence_met\"]"}));

  GuardFeatureService feature(nullptr);
  const Json::Value summary = drogon::sync_wait(
      feature.decisionsSummary({.from = 1799999999,
                                .to = 1800000001,
                                .nearMissMargin = 0}));
  CHECK(summary["totalRows"].asInt64() == 2);
  CHECK(summary["fired"].asInt64() == 1);
  CHECK(summary["legacyWould"].asInt64() == 2);
  CHECK(summary["beliefWould"].asInt64() == 2);
  CHECK(summary["since"].asInt64() == 1800000000);
  CHECK(summary["until"].asInt64() == 1800000000);
  REQUIRE(summary["bySeverity"].size() == 1);
  CHECK(summary["bySeverity"][0]["key"].asString() == "high");
  CHECK(summary["bySeverity"][0]["rows"].asInt64() == 2);
  CHECK(summary["bySeverity"][0]["fired"].asInt64() == 1);
  REQUIRE(summary["byMode"].size() == 2);
  REQUIRE(summary["scoreHistogram"].size() == 2);
  CHECK(summary["scoreHistogram"][0]["score"].asInt() == 3);
  CHECK(summary["scoreHistogram"][0]["fired"].asInt64() == 1);
  CHECK(summary["scoreHistogram"][1]["score"].asInt() == 6);
  CHECK(summary["scoreHistogram"][1]["beliefWould"].asInt64() == 1);
  bool dayFound = false;
  for (const auto& bucket : summary["byCameraDay"]) {
    if (bucket["cameraId"].asInt64() != 90)
      continue;
    dayFound = true;
    CHECK(bucket["events"].asInt64() == 2);
    CHECK(bucket["notified"].asInt64() == 1);
  }
  CHECK(dayFound);
  bool strongFound = false;
  bool persistFound = false;
  for (const auto& signal : summary["signals"]) {
    if (signal["signal"].asString() == "detector_strong") {
      strongFound = true;
      CHECK(signal["count"].asInt64() == 2);
    }
    if (signal["signal"].asString() == "persistence_met") {
      persistFound = true;
      CHECK(signal["count"].asInt64() == 1);
    }
  }
  CHECK(strongFound);
  CHECK(persistFound);
}

TEST_CASE("journal retention purges only old rows")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  GuardRepository repository;
  REQUIRE(drogon::sync_wait(repository.insertDecisionJournal(
      {.eventId = "rt:old",
       .encounterId = 1,
       .incidentId = 1,
       .cameraId = 92,
       .observationId = "92:1:100",
       .severity = "medium",
       .severityRank = 2,
       .hardFloor = false,
       .beliefScore = 5,
       .beliefSignals = "[]",
       .beliefThreshold = 5,
       .legacyWouldNotify = true,
       .beliefWouldNotify = true,
       .didNotify = true,
       .decisionMode = "shadow",
       .suppression = DecisionSuppression::None,
       .suppressedKinds = "[]",
       .noveltyScore = 0.0,
       .repeatVisits = 0,
       .quietHold = false,
       .budgetHold = false,
       .assessMs = 0,
       .createdAt = 100})));
  REQUIRE(drogon::sync_wait(repository.insertDecisionJournal(
      {.eventId = "rt:new",
       .encounterId = 2,
       .incidentId = 2,
       .cameraId = 92,
       .observationId = "92:2:100",
       .severity = "medium",
       .severityRank = 2,
       .hardFloor = false,
       .beliefScore = 5,
       .beliefSignals = "[]",
       .beliefThreshold = 5,
       .legacyWouldNotify = true,
       .beliefWouldNotify = true,
       .didNotify = true,
       .decisionMode = "shadow",
       .suppression = DecisionSuppression::None,
       .suppressedKinds = "[]",
       .noveltyScore = 0.0,
       .repeatVisits = 0,
       .quietHold = false,
       .budgetHold = false,
       .assessMs = 0,
       .createdAt = 1800000200})));
  CHECK(drogon::sync_wait(repository.purgeDecisions(1000)) == 1);
  CHECK(scalar("SELECT COUNT(*) FROM guard_decision_journal WHERE event_id = "
               "'rt:old'") == "0");
  CHECK(scalar("SELECT COUNT(*) FROM guard_decision_journal WHERE event_id = "
               "'rt:new'") == "1");
}

TEST_CASE("decisions query validation rejects bad inputs")
{
  auto request = drogon::HttpRequest::newHttpRequest();
  request->setParameter("limit", "200");
  request->setParameter("severity", "high");
  request->setParameter("divergent_only", "1");
  const auto valid = ListDecisionsDto::fromRequest(request);
  CHECK(valid.limit == 200);
  CHECK(valid.severity == "high");
  CHECK(valid.divergentOnly);

  auto clamped = drogon::HttpRequest::newHttpRequest();
  clamped->setParameter("limit", "99999");
  CHECK(ListDecisionsDto::fromRequest(clamped).limit == 200);

  int rejected = 0;
  for (const auto& [key, value] :
       std::vector<std::pair<std::string, std::string>>{
           {"severity", "bogus"},
           {"decision_mode", "bogus"},
           {"suppression_reason", "bogus"},
           {"limit", "0"},
           {"limit", "nan"},
           {"camera_id", "-5"}}) {
    auto bad = drogon::HttpRequest::newHttpRequest();
    bad->setParameter(key, value);
    try {
      ListDecisionsDto::fromRequest(bad);
    }
    catch (const ValidationException&) {
      ++rejected;
    }
  }
  CHECK(rejected == 6);

  auto halfCursor = drogon::HttpRequest::newHttpRequest();
  halfCursor->setParameter("after_event_id", "pg:1");
  bool cursorRejected = false;
  try {
    ListDecisionsDto::fromRequest(halfCursor);
  }
  catch (const ValidationException&) {
    cursorRejected = true;
  }
  CHECK(cursorRejected);

  auto summaryBad = drogon::HttpRequest::newHttpRequest();
  summaryBad->setParameter("from", "-3");
  bool summaryRejected = false;
  try {
    SummaryDecisionsDto::fromRequest(summaryBad);
  }
  catch (const ValidationException&) {
    summaryRejected = true;
  }
  CHECK(summaryRejected);
}

TEST_CASE("malformed signal payloads do not break the summary")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  DbService::client()->execSqlSync(
      "INSERT INTO guard_decision_journal (event_id, encounter_id, "
      "incident_id, camera_id, observation_id, severity, severity_rank, "
      "hard_floor, belief_score, belief_signals, belief_threshold, "
      "legacy_would_notify, belief_would_notify, did_notify, decision_mode, "
      "suppression_reason, suppressed_kinds, created_at) VALUES ('mx:1', 1, "
      "1, 93, '93:1:100', 'high', 3, 0, 4, 'not-json{{', 3, 1, 1, 0, "
      "'shadow', 'none', '[]', 1800000300)");

  GuardFeatureService feature(nullptr);
  const Json::Value summary = drogon::sync_wait(
      feature.decisionsSummary({.from = 1800000299,
                                .to = 1800000301,
                                .nearMissMargin = 0}));
  CHECK(summary["totalRows"].asInt64() == 1);
  CHECK(summary["signals"].size() == 0);
  CHECK(summary["unparseableSignalRows"].asInt64() == 1);
  CHECK(summary["bySeverity"].size() == 1);
}

TEST_CASE("near misses surface below-threshold rows inside a margin")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  GuardRepository repository;
  struct NearRowInput
  {
    std::string eventId;
    int score{0};
    int threshold{0};
    bool belief{false};
  };
  const auto insertRow = [&](const NearRowInput& row) {
    return drogon::sync_wait(repository.insertDecisionJournal(
        {.eventId = row.eventId,
         .encounterId = 1,
         .incidentId = 1,
         .cameraId = 95,
         .observationId = "95:1:100",
         .severity = "high",
         .severityRank = 3,
         .hardFloor = false,
         .beliefScore = row.score,
         .beliefSignals = "[]",
         .beliefThreshold = row.threshold,
         .legacyWouldNotify = true,
         .beliefWouldNotify = row.belief,
         .didNotify = false,
         .decisionMode = "shadow",
         .suppression = DecisionSuppression::None,
         .suppressedKinds = "[]",
         .noveltyScore = 0.0,
         .repeatVisits = 0,
         .quietHold = false,
         .budgetHold = false,
         .assessMs = 0,
         .createdAt = 1800000400}));
  };
  REQUIRE(insertRow({.eventId = "nm:1",
                      .score = 1,
                      .threshold = 3,
                      .belief = false}));
  REQUIRE(insertRow({.eventId = "nm:2",
                      .score = 2,
                      .threshold = 3,
                      .belief = false}));
  REQUIRE(insertRow(
      {.eventId = "nm:3", .score = 3, .threshold = 3, .belief = true}));

  GuardFeatureService feature(nullptr);
  DecisionsFilterInput wide;
  wide.limit = 200;
  wide.cameraId = 95;
  const DecisionsPage all = drogon::sync_wait(
      repository.listDecisionsFiltered(wide));
  CHECK(all.rows.size() == 3);

  DecisionsFilterInput near = wide;
  near.nearMissMargin = 2;
  const DecisionsPage misses =
      drogon::sync_wait(repository.listDecisionsFiltered(near));
  REQUIRE(misses.rows.size() == 2);
  CHECK(misses.rows[0].eventId == "nm:2");
  CHECK(misses.rows[1].eventId == "nm:1");

  const DecisionSummary summary = drogon::sync_wait(
      repository.summarizeDecisions({.from = 1800000399,
                                     .to = 1800000401,
                                     .nearMissMargin = 2}));
  CHECK(summary.nearMisses == 2);
  const DecisionSummary strict = drogon::sync_wait(
      repository.summarizeDecisions({.from = 1800000399,
                                     .to = 1800000401,
                                     .nearMissMargin = 0}));
  CHECK(strict.nearMisses == 0);
}

TEST_CASE("resident feedback labels one journal row")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  GuardRepository repository;
  REQUIRE(drogon::sync_wait(repository.insertDecisionJournal(
      {.eventId = "fb:1",
       .encounterId = 1,
       .incidentId = 1,
       .cameraId = 96,
       .observationId = "96:1:100",
       .severity = "medium",
       .severityRank = 2,
       .hardFloor = false,
       .beliefScore = 5,
       .beliefSignals = "[]",
       .beliefThreshold = 5,
       .legacyWouldNotify = true,
       .beliefWouldNotify = true,
       .didNotify = true,
       .decisionMode = "shadow",
       .suppression = DecisionSuppression::None,
       .suppressedKinds = "[]",
       .noveltyScore = 0.0,
       .repeatVisits = 0,
       .quietHold = false,
       .budgetHold = false,
       .assessMs = 0,
       .createdAt = 1800000500})));

  GuardFeatureService feature(nullptr);
  CHECK_FALSE(drogon::sync_wait(feature.setFeedback("fb:1", "bogus")));
  CHECK_FALSE(drogon::sync_wait(feature.setFeedback("fb:missing", "useful")));
  CHECK(drogon::sync_wait(feature.setFeedback("fb:1", "false_alarm")));
  CHECK(scalar("SELECT feedback_label FROM guard_decision_journal WHERE "
               "event_id = 'fb:1'") == "false_alarm");
  CHECK(scalar("SELECT feedback_at FROM guard_decision_journal WHERE "
               "event_id = 'fb:1'") != "0");

  const Json::Value decisions = drogon::sync_wait(feature.decisions(
      {.limit = 200,
       .from = 1800000500,
       .to = 1800000500,
       .cameraId = 96,
       .severity = {},
       .decisionMode = {},
       .suppressionReason = {},
       .divergentOnly = false,
       .nearMissMargin = 0,
       .afterCreatedAt = 0,
       .afterEventId = {}}));
  REQUIRE(decisions["rows"].size() == 1);
  CHECK(decisions["rows"][0]["feedbackLabel"].asString() == "false_alarm");

  auto bad = drogon::HttpRequest::newHttpRequest();
  bad->setParameter("near_miss_margin", "-1");
  bool marginRejected = false;
  try {
    ListDecisionsDto::fromRequest(bad);
  }
  catch (const ValidationException&) {
    marginRejected = true;
  }
  CHECK(marginRejected);

  auto summaryMarginBad = drogon::HttpRequest::newHttpRequest();
  summaryMarginBad->setParameter("near_miss_margin", "nope");
  bool summaryMarginRejected = false;
  try {
    SummaryDecisionsDto::fromRequest(summaryMarginBad);
  }
  catch (const ValidationException&) {
    summaryMarginRejected = true;
  }
  CHECK(summaryMarginRejected);

  const Json::Value missingLabel(Json::objectValue);
  bool labelRejected = false;
  try {
    FeedbackDecisionDto::fromJson(missingLabel);
  }
  catch (const ValidationException&) {
    labelRejected = true;
  }
  CHECK(labelRejected);
  Json::Value bogusLabel(Json::objectValue);
  bogusLabel["label"] = "maybe";
  bool bogusRejected = false;
  try {
    FeedbackDecisionDto::fromJson(bogusLabel);
  }
  catch (const ValidationException&) {
    bogusRejected = true;
  }
  CHECK(bogusRejected);
  Json::Value goodLabel(Json::objectValue);
  goodLabel["label"] = "useful";
  CHECK(FeedbackDecisionDto::fromJson(goodLabel).label == "useful");
}

TEST_CASE("decision rows carry a human summary")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  GuardRepository repository;
  REQUIRE(drogon::sync_wait(repository.insertDecisionJournal(
      {.eventId = "hs:1",
       .encounterId = 1,
       .incidentId = 1,
       .cameraId = 94,
       .observationId = "94:1:100",
       .severity = "high",
       .severityRank = 3,
       .hardFloor = false,
       .beliefScore = 4,
       .beliefSignals =
           "[\"detector_strong\",\"persistence_met\",\"identity_unrecognized\"]",
       .beliefThreshold = 5,
       .legacyWouldNotify = true,
       .beliefWouldNotify = false,
       .didNotify = false,
       .decisionMode = "shadow",
       .suppression = DecisionSuppression::BeliefGate,
       .suppressedKinds = "[\"notify\"]",
       .noveltyScore = 0.0,
       .repeatVisits = 0,
       .quietHold = false,
       .budgetHold = false,
       .assessMs = 0,
       .createdAt = 1800000600})));

  GuardFeatureService feature(nullptr);
  const Json::Value decisions = drogon::sync_wait(feature.decisions(
      {.limit = 200,
       .from = 1800000600,
       .to = 1800000600,
       .cameraId = 94,
       .severity = {},
       .decisionMode = {},
       .suppressionReason = {},
       .divergentOnly = false,
       .nearMissMargin = 0,
       .afterCreatedAt = 0,
       .afterEventId = {}}));
  REQUIRE(decisions["rows"].size() == 1);
  CHECK(decisions["rows"][0]["summary"].asString() ==
        "high event, camera 94: unrecognized person (strong detection, "
        "lingering). Score 4 vs threshold 5 — held: belief below threshold.");
  CHECK(decisions["rows"][0]["dispatchAttempts"].asInt() == 0);
  CHECK(decisions["rows"][0]["quietHold"].asBool() == false);

  DbService::client()->execSqlSync(
      "UPDATE guard_decision_journal SET dispatch_attempts = 3 WHERE event_id "
      "= 'hs:1'");
  const Json::Value reread = drogon::sync_wait(feature.decisions(
      {.limit = 200,
       .from = 1800000600,
       .to = 1800000600,
       .cameraId = 94,
       .severity = {},
       .decisionMode = {},
       .suppressionReason = {},
       .divergentOnly = false,
       .nearMissMargin = 0,
       .afterCreatedAt = 0,
       .afterEventId = {}}));
  REQUIRE(reread["rows"].size() == 1);
  CHECK(reread["rows"][0]["dispatchAttempts"].asInt() == 3);

  const DecisionSummary summary = drogon::sync_wait(
      repository.summarizeDecisions({.from = 1800000600,
                                     .to = 1800000600,
                                     .nearMissMargin = 0}));
  CHECK(summary.quietHeld == 0);
  CHECK(summary.budgetHeld == 0);
  CHECK(summary.assessMsP50 == 0);
  CHECK(summary.assessMsP95 == 0);
}

TEST_CASE("holds and baselines surface for calibration")
{
  SharedBoot& boot = sharedBoot();
  (void)boot;
  GuardRepository repository;
  REQUIRE(drogon::sync_wait(repository.insertDecisionJournal(
      {.eventId = "hb:1",
       .encounterId = 1,
       .incidentId = 1,
       .cameraId = 97,
       .observationId = "97:1:100",
       .severity = "medium",
       .severityRank = 2,
       .hardFloor = false,
       .beliefScore = 5,
       .beliefSignals = "[]",
       .beliefThreshold = 5,
       .legacyWouldNotify = true,
       .beliefWouldNotify = true,
       .didNotify = true,
       .decisionMode = "shadow",
       .suppression = DecisionSuppression::None,
       .suppressedKinds = "[]",
       .noveltyScore = 0.25,
       .repeatVisits = 3,
       .quietHold = true,
       .budgetHold = true,
       .assessMs = 0,
       .createdAt = 1800000700})));

  const DecisionSummary summary = drogon::sync_wait(
      repository.summarizeDecisions({.from = 1800000699,
                                     .to = 1800000701,
                                     .nearMissMargin = 0}));
  CHECK(summary.quietHeld == 1);
  CHECK(summary.budgetHeld == 1);

  GuardFeatureService feature(nullptr);
  const Json::Value windowed = drogon::sync_wait(feature.decisionsSummary(
      {.from = 1800000699, .to = 1800000701, .nearMissMargin = 0}));
  CHECK(windowed["quietHeld"].asInt64() == 1);
  CHECK(windowed["budgetHeld"].asInt64() == 1);
  CHECK(windowed["nearMisses"].asInt64() == 0);
  CHECK(windowed["assessMsP50"].asInt64() == 0);
  CHECK(windowed["assessMsP95"].asInt64() == 0);
  REQUIRE(windowed["detectionHealth"].isObject());
  CHECK(windowed["detectionHealth"]["thermalC"].isDouble());
  CHECK(windowed["detectionHealth"]["thermalBucket"].isString());

  const BaselineEmaRow cold = drogon::sync_wait(repository.baselineEma(97, 41));
  CHECK(cold.ema == 0.0);
  CHECK(cold.updatedAt == 0);
  REQUIRE(drogon::sync_wait(
      repository.upsertBaselineEma({.cameraId = 97,
                                    .dowHour = 41,
                                    .ema = 0.5,
                                    .at = 1800000700})));
  const BaselineEmaRow written =
      drogon::sync_wait(repository.baselineEma(97, 41));
  CHECK(written.ema == 0.5);
  CHECK(written.updatedAt == 1800000700);
  CHECK(drogon::sync_wait(repository.touchSignatureVisit({}, 1)) == 0);
  CHECK(drogon::sync_wait(repository.touchSignatureVisit("hb-sig", 1)) == 1);
  CHECK(drogon::sync_wait(repository.touchSignatureVisit("hb-sig", 2)) == 2);
  CHECK(drogon::sync_wait(repository.firedSince(1800000699)) == 1);

  REQUIRE(drogon::sync_wait(
      repository.setDecisionFeedback({.eventId = "hb:1",
                                      .label = "useful",
                                      .at = 1800000701})));
  const DecisionsFilterInput filter{.limit = 200,
                                    .from = 1800000699,
                                    .to = 1800000701,
                                    .cameraId = 97,
                                    .severity = {},
                                    .decisionMode = {},
                                    .suppressionReason = {},
                                    .divergentOnly = false,
                                    .nearMissMargin = 0,
                                    .afterCreatedAt = 0,
                                    .afterEventId = {}};
  const DecisionsPage page =
      drogon::sync_wait(repository.listDecisionsFiltered(filter));
  REQUIRE(page.rows.size() == 1);
  CHECK(page.rows.front().quietHold);
  CHECK(page.rows.front().budgetHold);
  CHECK(page.rows.front().noveltyScore == 0.25);
  CHECK(page.rows.front().repeatVisits == 3);
  CHECK(page.rows.front().feedbackLabel == "useful");
}
