#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "guard-fakes.hxx"

#include <cstdio>
#include <doctest/doctest.h>
#include <drogon/drogon.h>
#include <feature/guard/dtos/feedback-decision-dto.hxx>
#include <feature/guard/guard-repository.hxx>
#include <feature/guard/guard-service.hxx>
#include <feature/guard/infra/resource-change-outbox.hxx>
#include <feature/guard/services/episode-change-gate.hxx>
#include <feature/guard/services/guard-feature-service.hxx>
#include <feature/guard/vocabulary/feedback-label.hxx>
#include <memory>
#include <nats/nats-bus.hxx>
#include <sqlite/db-service.hxx>
#include <string>

using guard_test::GuardBoot;

namespace
{
int64_t scalarCount(const std::string& sql)
{
  const auto rows = DbService::client()->execSqlSync(sql);
  return rows.empty() ? 0 : rows[0][0].as<int64_t>();
}

GuardBoot& sharedBoot()
{
  static GuardBoot boot("guard-episode-change-test");
  return boot;
}

void clearGuardTables()
{
  DbService::client()->execSqlSync("DELETE FROM change_outbox");
  DbService::client()->execSqlSync("DELETE FROM guard_encounter_transition");
  DbService::client()->execSqlSync("DELETE FROM guard_encounter");
  DbService::client()->execSqlSync("DELETE FROM guard_observation_inbox");
}

int64_t changeRows()
{
  return scalarCount("SELECT COUNT(*) FROM change_outbox");
}

std::string lastPayload()
{
  const auto rows = DbService::client()->execSqlSync(
      "SELECT payload FROM change_outbox ORDER BY rowid DESC LIMIT 1");
  return rows.empty() ? std::string{} : rows[0][0].as<std::string>();
}

std::string idsOf(std::initializer_list<int64_t> ids)
{
  std::string text = "\"ids\":[";
  bool first = true;
  for (const auto id : ids) {
    if (!first)
      text += ',';
    first = false;
    text += std::to_string(id);
  }
  return text + "]";
}

int64_t insertEncounter(int64_t firstSeen, int64_t lastSeen,
                        const std::string& state = "observing")
{
  DbService::client()->execSqlSync(
      "INSERT INTO guard_encounter (person_id, signature, state, grade, "
      "best_camera_id, first_seen, last_seen) VALUES (0, '', '" +
      state + "', 'medium', 1, " + std::to_string(firstSeen) + ", " +
      std::to_string(lastSeen) + ")");
  return scalarCount("SELECT MAX(id) FROM guard_encounter");
}
}

TEST_CASE("creating an episode through an observation announces it once")
{
  sharedBoot();
  clearGuardTables();
  const auto bus = std::make_shared<NatsBus>();
  const ResourceChangeOutbox outbox(bus);
  const GuardRepository repository;

  const int64_t before = changeRows();
  const GuardEncounterPhaseResult phase = drogon::sync_wait(
      repository.commitEncounterPhase(
          {.create = true,
           .encounterIdToTouch = 0,
           .createInput = {.personId = 7,
                           .signature = "sig",
                           .bestCameraId = 3,
                           .bestScore = 0.9,
                           .at = 1000},
           .touchInput = {},
           .closeForPerson = false,
           .closePersonId = 0,
           .closeAt = 0,
           .advance = {.eventId = "evt-1",
                       .stage = 1,
                       .incidentId = 0,
                       .encounterId = 0,
                       .danger = "medium",
                       .checkpoint = "{}",
                       .at = 1000},
           .match = std::nullopt}));
  CHECK(phase.committed);
  CHECK(phase.created);
  CHECK(phase.encounterId > 0);
  CHECK(changeRows() == before + 1);
  CHECK(lastPayload().find("\"scope\":\"episodes\"") != std::string::npos);
  CHECK(lastPayload().find(idsOf({phase.encounterId})) != std::string::npos);
}

TEST_CASE("a transition and a stale close announce the episodes they moved")
{
  sharedBoot();
  clearGuardTables();
  const auto bus = std::make_shared<NatsBus>();
  const ResourceChangeOutbox outbox(bus);
  const GuardRepository repository;
  const int64_t encounter = insertEncounter(1000, 1000);

  const int64_t before = changeRows();
  CHECK(drogon::sync_wait(repository.transitionEncounter(
      {.encounterId = encounter,
       .toState = EncounterState::Resolved,
       .reason = "loiter",
       .grade = "high",
       .at = 1200})));
  CHECK(changeRows() == before + 1);
  CHECK(lastPayload().find(idsOf({encounter})) != std::string::npos);

  CHECK(drogon::sync_wait(repository.transitionEncounter(
      {.encounterId = encounter,
       .toState = EncounterState::Resolved,
       .reason = "loiter",
       .grade = "high",
       .at = 1300})));
  CHECK(changeRows() == before + 1);

  CHECK(drogon::sync_wait(repository.closeStaleEncounters(
      {.olderThan = 2000, .closedAt = 2100, .decisionMode = "shadow"})));
  CHECK(changeRows() == before + 3);
  CHECK(lastPayload().find("\"scope\":\"episodes\"") != std::string::npos);
  CHECK(lastPayload().find(idsOf({encounter})) != std::string::npos);
}

TEST_CASE("a review and a retain announce the episode they touch")
{
  sharedBoot();
  clearGuardTables();
  const auto bus = std::make_shared<NatsBus>();
  const ResourceChangeOutbox outbox(bus);
  const GuardFeatureService service(
      {.identity = nullptr, .disarm = nullptr, .changes = &outbox});
  const int64_t encounter = insertEncounter(1000, 1000);

  const int64_t before = changeRows();
  const auto reviewed = drogon::sync_wait(service.reviewEpisode(
      {.episodeId = encounter, .label = FeedbackLabel::FalseAlarm}));
  REQUIRE(reviewed.has_value());
  CHECK(changeRows() == before + 1);
  CHECK(lastPayload().find(idsOf({encounter})) != std::string::npos);

  const auto retained = drogon::sync_wait(
      service.retainEpisode({.episodeId = encounter, .retain = true}));
  REQUIRE(retained.has_value());
  CHECK(changeRows() == before + 2);

  CHECK_FALSE(drogon::sync_wait(service.reviewEpisode(
                  {.episodeId = encounter + 500, .label = FeedbackLabel::Useful}))
                  .has_value());
  CHECK(changeRows() == before + 2);
}

TEST_CASE("a burst of touches announces once, and once more when the burst ends")
{
  sharedBoot();
  clearGuardTables();
  const auto bus = std::make_shared<NatsBus>();
  const ResourceChangeOutbox outbox(bus);
  int64_t at = 1'700'000'000;
  EpisodeChangeGate gate(outbox,
                         {.publishSeconds = 15,
                          .clock = [&at]() { return at; }});

  const int64_t encounter = insertEncounter(1000, 1000);
  const int64_t before = changeRows();
  for (int step = 0; step < 5; ++step) {
    drogon::sync_wait(gate.touch(encounter));
    at += 1;
  }
  CHECK(changeRows() == before + 1);
  CHECK(lastPayload().find(idsOf({encounter})) != std::string::npos);

  drogon::sync_wait(gate.flush());
  CHECK(changeRows() == before + 1);

  at += 15;
  drogon::sync_wait(gate.flush());
  CHECK(changeRows() == before + 2);
  CHECK(lastPayload().find(idsOf({encounter})) != std::string::npos);

  drogon::sync_wait(gate.flush());
  CHECK(changeRows() == before + 2);

  drogon::sync_wait(gate.touch(encounter));
  CHECK(changeRows() == before + 2);
}

TEST_CASE("a single touch announces once and leaves nothing trailing")
{
  sharedBoot();
  clearGuardTables();
  const auto bus = std::make_shared<NatsBus>();
  const ResourceChangeOutbox outbox(bus);
  int64_t at = 1'700'000'000;
  EpisodeChangeGate gate(outbox,
                         {.publishSeconds = 15,
                          .clock = [&at]() { return at; }});

  const int64_t encounter = insertEncounter(1000, 1000);
  const int64_t before = changeRows();
  drogon::sync_wait(gate.touch(encounter));
  CHECK(changeRows() == before + 1);

  at += 300;
  drogon::sync_wait(gate.flush());
  CHECK(changeRows() == before + 1);

  at += 300;
  drogon::sync_wait(gate.touch(encounter));
  CHECK(changeRows() == before + 2);
}

TEST_CASE("an observation announces its episode, and the gate quiets the rest")
{
  (void)sharedBoot();
  clearGuardTables();
  const auto bus = std::make_shared<NatsBus>();
  const ResourceChangeOutbox outbox(bus);
  guard_test::RosterIdentity identity({{1, "es"}});
  guard_test::RecordingNotifications notifications;
  guard_test::QuietCameraActions camera;
  int64_t at = 1'700'000'000;
  EpisodeChangeGate gate(outbox,
                         {.publishSeconds = 15,
                          .clock = [&at]() { return at; }});
  GuardService service({.bus = nullptr,
                        .identity = &identity,
                        .notifications = &notifications,
                        .actions = &camera,
                        .assessment = nullptr,
                        .directory = {},
                        .active = {},
                        .episodeChanges = &gate},
                       {});
  const auto observation = [](const std::string& eventId, int trackId) {
    Json::Value event(Json::objectValue);
    event["schemaVersion"] = 3;
    event["eventId"] = eventId;
    event["cameraId"] = static_cast<Json::Int64>(1);
    event["cameraName"] = "Entrada";
    event["rule"] = "person";
    event["severity"] = "medium";
    event["escalated"] = false;
    event["night"] = false;
    event["trackId"] = static_cast<Json::Int64>(trackId);
    event["publishedAt"] = static_cast<Json::Int64>(1'700'000'000'000);
    Json::Value object(Json::objectValue);
    object["class"] = "person";
    object["confidence"] = 0.9;
    object["identityState"] = "unobservable";
    object["identifyAttempts"] = 2;
    object["scoreMedian"] = 0.0;
    object["scoreSamples"] = 0;
    object["zoneWindows"] = 3;
    object["trackWindows"] = 4;
    object["areaSpread"] = 1.2;
    object["trackId"] = static_cast<Json::Int64>(trackId);
    object["firstSeenMs"] = static_cast<Json::Int64>(1'699'999'982'000);
    object["lastSeenMs"] = static_cast<Json::Int64>(1'700'000'000'000);
    object["dwellMs"] = static_cast<Json::Int64>(18'000);
    object["observationId"] = "1:" + std::to_string(trackId) + ":1";
    object["bbox"] = Json::Value(Json::objectValue);
    Json::Value objects(Json::arrayValue);
    objects.append(object);
    event["objects"] = std::move(objects);
    return event;
  };

  const int64_t before = changeRows();
  CHECK(drogon::sync_wait(service.handle(observation("obs-1", 4), 1)));
  CHECK(changeRows() == before + 1);
  CHECK(lastPayload().find("\"scope\":\"episodes\"") != std::string::npos);

  at += 1;
  CHECK(drogon::sync_wait(service.handle(observation("obs-2", 4), 1)));
  CHECK(changeRows() == before + 1);

  drogon::sync_wait(gate.flush());
  CHECK(changeRows() == before + 1);

  at += 15;
  drogon::sync_wait(gate.flush());
  CHECK(changeRows() == before + 2);
  CHECK(lastPayload().find("\"scope\":\"episodes\"") != std::string::npos);
}

TEST_CASE("two changes to one episode in the same instant get their own notice")
{
  sharedBoot();
  clearGuardTables();
  const auto bus = std::make_shared<NatsBus>();
  const ResourceChangeOutbox outbox(bus);
  const GuardRepository repository;
  const int64_t encounter = insertEncounter(1000, 1000);

  const int64_t before = changeRows();
  CHECK(drogon::sync_wait(repository.transitionEncounter(
      {.encounterId = encounter,
       .toState = EncounterState::Verifying,
       .reason = "loiter",
       .grade = "medium",
       .at = 1200})));
  CHECK(drogon::sync_wait(repository.transitionEncounter(
      {.encounterId = encounter,
       .toState = EncounterState::Resolved,
       .reason = "left",
       .grade = "medium",
       .at = 1200})));
  CHECK(changeRows() == before + 2);
  CHECK(scalarCount("SELECT COUNT(DISTINCT event_id) FROM change_outbox") == 2);
}

TEST_CASE("a purge announces the episodes scope without ids")
{
  sharedBoot();
  clearGuardTables();
  const auto bus = std::make_shared<NatsBus>();
  const ResourceChangeOutbox outbox(bus);
  const GuardRepository repository;
  insertEncounter(1000, 1000, "closed");

  const int64_t before = changeRows();
  CHECK(drogon::sync_wait(repository.purgeHistory(
            {.historyBefore = 10'000, .inboxBefore = 10'000})) > 0);
  CHECK(changeRows() == before + 1);
  CHECK(lastPayload().find("\"scope\":\"episodes\"") != std::string::npos);
  CHECK(lastPayload().find("\"ids\"") == std::string::npos);

  const int64_t after = changeRows();
  CHECK(drogon::sync_wait(repository.purgeHistory(
            {.historyBefore = 10'000, .inboxBefore = 10'000})) == 0);
  CHECK(changeRows() == after);
}
