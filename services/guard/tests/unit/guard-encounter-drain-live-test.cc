#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <feature/guard/guard-query.hxx>
#include <feature/guard/guard-repository.hxx>
#include <feature/guard/guard-schema.hxx>
#include <feature/guard/guard-service.hxx>
#include <nats/nats-bus.hxx>
#include <sqlite/db-service.hxx>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>

#include "temp-db.hxx"
#include "wait-for-boot.hxx"

using guard_test::TempDb;
using guard_test::waitForBoot;
using namespace guard_query;

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

std::string scalar(const std::string& sql)
{
  const auto rows = DbService::client()->execSqlSync(sql);
  if (rows.empty())
    return {};
  return rows.front()[0].as<std::string>();
}

NatsBus::StreamStatus
streamStatus(const std::optional<NatsBus::StreamStatus>& status)
{
  REQUIRE(status.has_value());
  return status.value_or(NatsBus::StreamStatus{});
}

struct EncounterCloseRequest
{
  int64_t personId{0};
  int64_t at{0};
  std::string stageId;
};

std::string closeEncounter(GuardRepository& repository,
                           const EncounterCloseRequest& request)
{
  const int64_t closedAt = request.at + 100;
  const GuardEncounterPhaseInput createPhase =
      {.create = true,
       .encounterIdToTouch = 0,
       .createInput = {.personId = request.personId,
                       .signature = {},
                       .bestCameraId = 3,
                       .bestScore = 1.0,
                       .at = request.at},
       .touchInput = {},
       .closeForPerson = false,
       .closePersonId = 0,
       .closeAt = 0,
       .advance = {.eventId = request.stageId,
                   .stage = 2,
                   .incidentId = 0,
                   .encounterId = 0,
                   .danger = "none",
                   .checkpoint = "{}",
                   .at = request.at}};
  const auto created =
      drogon::sync_wait(repository.commitEncounterPhase(createPhase));
  REQUIRE(created.committed);
  const GuardEncounterPhaseInput closePhase =
      {.create = false,
       .encounterIdToTouch = created.encounterId,
       .createInput = {},
       .touchInput = {.id = created.encounterId,
                      .bestCameraId = 3,
                      .bestScore = 1.0,
                      .lastSeen = closedAt},
       .closeForPerson = true,
       .closePersonId = request.personId,
       .closeAt = closedAt,
       .advance = {.eventId = request.stageId + ":close",
                   .stage = 3,
                   .incidentId = 0,
                   .encounterId = created.encounterId,
                   .danger = "none",
                   .checkpoint = "{}",
                   .at = closedAt}};
  const auto closed =
      drogon::sync_wait(repository.commitEncounterPhase(closePhase));
  REQUIRE(closed.committed);
  REQUIRE(closed.closed.size() == 1);
  return encounterClosedEventId(closed.closed.front(), closedAt);
}
}

TEST_CASE("the encounter drain creates its own stream and settles what it publishes")
{
  const char* url = std::getenv("ARGUS_NATS_URL");
  if (url == nullptr || *url == '\0') {
    MESSAGE("ARGUS_NATS_URL not set; guard encounter drain check skipped");
    return;
  }

  const TempDb db("guard-encounter-drain-test");
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, db.path(), "default", -1});
  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));
  REQUIRE(DbService::runScriptFile(ARGUS_GUARD_SCHEMA_PATH));

  NatsBus bus;
  NatsBus::Options options;
  options.url = url;
  options.reconnectWaitMs = 200;
  options.maxReconnects = 5;
  REQUIRE(bus.connect(options));

  GuardService::Config config;
  config.enabled = true;
  const std::string run =
      std::to_string(::getpid()) + "-" +
      std::to_string(
          std::chrono::steady_clock::now().time_since_epoch().count());
  const std::string family = "argus.test.guard.drain." + run;
  config.guardStream = "ARGUS_GUARD_DRAIN_" + run;
  config.guardSubjectFilter = family + ".>";
  config.guardEncounterSubject = family + ".encounter_closed";
  CHECK_FALSE(bus.streamInfo(config.guardStream).has_value());
  GuardRepository repository;
  const std::string first =
      closeEncounter(repository, {.personId = 42, .at = 700, .stageId = "drain:1"});
  CHECK(scalar("SELECT status FROM guard_encounter_outbox WHERE event_id = '" +
               first + "'") == "pending");

  GuardService service({.bus = &bus,
                        .identity = nullptr,
                        .notifications = nullptr,
                        .actions = nullptr,
                        .assessment = nullptr},
                       config);
  drogon::sync_wait(service.flushEncounterOutbox());

  CHECK(scalar("SELECT status FROM guard_encounter_outbox WHERE event_id = '" +
               first + "'") == "sent");
  CHECK(scalar("SELECT attempts FROM guard_encounter_outbox WHERE event_id = '" +
               first + "'") == "0");
  const auto created = streamStatus(bus.streamInfo(config.guardStream));
  CHECK(std::ranges::find(created.subjects, family + ".>") !=
        created.subjects.end());

  const std::string second =
      closeEncounter(repository, {.personId = 43, .at = 900, .stageId = "drain:2"});
  drogon::sync_wait(service.flushEncounterOutbox());
  CHECK(scalar("SELECT status FROM guard_encounter_outbox WHERE event_id = '" +
               second + "'") == "sent");
  CHECK(scalar("SELECT attempts FROM guard_encounter_outbox WHERE event_id = '" +
               second + "'") == "0");
  CHECK(scalar("SELECT COUNT(*) FROM guard_encounter_outbox WHERE status = "
               "'pending'") == "0");

  bus.drain();
}
