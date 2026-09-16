#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <guard-schema.hxx>
#include <guard-service.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/utils/json-util/json-util.hxx>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <unistd.h>

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

Json::Value poisonObservation(const std::string& eventId)
{
  Json::Value event(Json::objectValue);
  event["schemaVersion"] = 2;
  event["eventId"] = eventId;
  event["cameraId"] = 1;
  event["rule"] = "person_day";
  event["severity"] = "info";
  event["trackId"] = Json::Int64(1);
  Json::Value object(Json::objectValue);
  object["class"] = "person";
  object["identity"] = "unknown";
  object["trackId"] = Json::Int64(1);
  Json::Value bbox(Json::objectValue);
  bbox["w"] = 32.0;
  bbox["h"] = 32.0;
  object["bbox"] = bbox;
  Json::Value objects(Json::arrayValue);
  objects.append(object);
  event["objects"] = objects;
  return event;
}
} // namespace

TEST_CASE("a failing dead-letter commit leaves no partial park and resumes")
{
  const TempDb db("guard-dlq-crash-test");
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, db.path(), "default", -1});
  std::thread runner([] { drogon::app().run(); });
  REQUIRE(waitForBoot(std::chrono::seconds(30)));
  REQUIRE(DbService::runScriptFile(ARGUS_GUARD_SCHEMA_PATH));

  GuardService::Config config;
  config.enabled = true;
  config.maxObservationAttempts = 3;
  config.greetEnabled = false;
  config.stagingEnabled = false;
  GuardService service({.bus = nullptr,
                        .identity = nullptr,
                        .notifications = nullptr,
                        .actions = nullptr,
                        .assessment = nullptr},
                       config);

  DbService::client()->execSqlSync("DROP TABLE guard_dead_letter");
  bool threw = false;
  try {
    drogon::sync_wait(service.handle(poisonObservation("park-crash:1"), 2));
  }
  catch (const std::exception&) {
    threw = true;
  }
  CHECK(threw);
  REQUIRE(DbService::runScriptFile(ARGUS_GUARD_SCHEMA_PATH));
  CHECK(scalar("SELECT COUNT(*) FROM guard_dead_letter") == "0");
  CHECK(scalar("SELECT status FROM guard_observation_inbox "
               "WHERE event_id = 'park-crash:1'") == "processing");

  REQUIRE(drogon::sync_wait(service.handle(poisonObservation("park-crash:1"), 3)));
  CHECK(scalar("SELECT COUNT(*) FROM guard_dead_letter WHERE "
               "event_id = 'park-crash:1'") == "1");
  CHECK(scalar("SELECT attempts FROM guard_dead_letter WHERE "
               "event_id = 'park-crash:1'") == "3");
  CHECK(scalar("SELECT status FROM guard_observation_inbox "
               "WHERE event_id = 'park-crash:1'") == "dead_lettered");

  REQUIRE(drogon::sync_wait(service.handle(poisonObservation("park-crash:1"), 4)));
  CHECK(scalar("SELECT COUNT(*) FROM guard_dead_letter WHERE "
               "event_id = 'park-crash:1'") == "1");

  drogon::app().quit();
  runner.join();
}
