#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <chrono>
#include <cstdio>
#include <doctest/doctest.h>
#include <drogon/drogon.h>
#include <feature/guard/dtos/create-environment-dto.hxx>
#include <feature/guard/dtos/update-camera-context-dto.hxx>
#include <feature/guard/dtos/update-environment-dto.hxx>
#include <feature/guard/infra/resource-change-outbox.hxx>
#include <feature/guard/services/guard-feature-service.hxx>
#include <memory>
#include <nats/nats-bus.hxx>
#include <optional>
#include <sqlite/db-service.hxx>
#include <stdexcept>
#include <string>
#include <test-support/app-runner.hxx>

#include "temp-db.hxx"
#include "wait-for-boot.hxx"

using guard_test::TempDb;
using guard_test::waitForBoot;
using test_support::AppRunner;

namespace
{
int64_t scalarCount(const std::string& sql)
{
  const auto rows = DbService::client()->execSqlSync(sql);
  return rows.empty() ? 0 : rows[0][0].as<int64_t>();
}

std::string scalarText(const std::string& sql)
{
  const auto rows = DbService::client()->execSqlSync(sql);
  return rows.empty() ? std::string{} : rows[0][0].as<std::string>();
}

struct SharedBoot
{
  TempDb db{"guard-environment-change-test"};
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

void clearGuardTables()
{
  DbService::client()->execSqlSync("DELETE FROM change_outbox");
  DbService::client()->execSqlSync("DELETE FROM guard_state");
  DbService::client()->execSqlSync("DELETE FROM guard_camera_context");
  DbService::client()->execSqlSync("DELETE FROM guard_expected_guest");
  DbService::client()->execSqlSync("DELETE FROM guard_environment");
}

int64_t insertDefaultEnvironment()
{
  DbService::client()->execSqlSync(
      "INSERT INTO guard_environment (name, kind, is_default, mode, "
      "schedule_enabled, asleep_hours, closed_mode) VALUES ('Default house', "
      "'home', 1, 'home', 0, '', 'away')");
  return scalarCount("SELECT id FROM guard_environment WHERE is_default = 1");
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

Json::Value createEnvironment(const GuardFeatureService& service,
                              const std::string& name)
{
  return drogon::sync_wait(service.createEnvironment(
      {.fields = UpdateEnvironmentDto{.name = name,
                                      .kind = "home",
                                      .scheduleEnabled = false,
                                      .asleep = "",
                                      .open = "",
                                      .staffed = "",
                                      .closedMode = "away",
                                      .digestHour = 21,
                                      .quietPolicy = "inherit",
                                      .quietStartHour = 22,
                                      .quietEndHour = 7,
                                      .lanPresence = false,
                                      .invalidTypes = ""},
       .mode = "home"}));
}

int64_t postureRow(int64_t environmentId)
{
  return scalarCount("SELECT COUNT(*) FROM guard_state WHERE key = '" +
                     guard_schedule::stateKey(environmentId) + "'");
}

std::string idsOf(std::initializer_list<int64_t> ids)
{
  std::string text = "\"ids\":[";
  bool first = true;
  for (const auto id : ids) {
    if (!first)
      text += ",";
    first = false;
    text += std::to_string(id);
  }
  return text + "]";
}
}

TEST_CASE("creating an environment announces it and stores its posture")
{
  sharedBoot();
  clearGuardTables();
  const auto bus = std::make_shared<NatsBus>();
  const ResourceChangeOutbox outbox(bus, ResourceChangeOutbox::Config{});
  const GuardFeatureService service(
      {.identity = nullptr, .disarm = nullptr, .changes = &outbox});

  const Json::Value created = createEnvironment(service, "Change house");
  const int64_t id = created["id"].asInt64();
  CHECK(id > 0);
  CHECK(changeRows() == 1);
  CHECK(lastPayload().find("\"scope\":\"environments\"") != std::string::npos);
  CHECK(lastPayload().find(idsOf({id})) != std::string::npos);
  CHECK(postureRow(id) == 1);
}

TEST_CASE("editing an environment announces it and leaves its mode alone")
{
  sharedBoot();
  clearGuardTables();
  const auto bus = std::make_shared<NatsBus>();
  const ResourceChangeOutbox outbox(bus, ResourceChangeOutbox::Config{});
  const GuardFeatureService service(
      {.identity = nullptr, .disarm = nullptr, .changes = &outbox});

  const int64_t id = createEnvironment(service, "Edit house")["id"].asInt64();
  const int64_t before = changeRows();

  const Json::Value updated = drogon::sync_wait(service.updateEnvironment(
      {.id = id,
       .patch = UpdateEnvironmentDto{.name = "Edit house renamed",
                                     .kind = std::nullopt,
                                     .scheduleEnabled = std::nullopt,
                                     .asleep = std::nullopt,
                                     .open = std::nullopt,
                                     .staffed = std::nullopt,
                                     .closedMode = std::nullopt,
                                     .digestHour = 8,
                                     .quietPolicy = std::nullopt,
                                     .quietStartHour = std::nullopt,
                                     .quietEndHour = std::nullopt,
                                     .lanPresence = std::nullopt,
                                     .invalidTypes = ""}}));
  CHECK(updated["name"].asString() == "Edit house renamed");
  CHECK(updated["mode"].asString() == "home");
  CHECK(changeRows() == before + 1);
  CHECK(lastPayload().find(idsOf({id})) != std::string::npos);
  CHECK(scalarText("SELECT mode FROM guard_environment WHERE id = '" +
                   std::to_string(id) + "'") == "home");
}

TEST_CASE("removing an environment announces it and hands its cameras away")
{
  sharedBoot();
  clearGuardTables();
  const auto bus = std::make_shared<NatsBus>();
  const ResourceChangeOutbox outbox(bus, ResourceChangeOutbox::Config{});
  const GuardFeatureService service(
      {.identity = nullptr, .disarm = nullptr, .changes = &outbox});

  const int64_t kept = insertDefaultEnvironment();
  const int64_t doomed = createEnvironment(service, "Doomed shop")["id"].asInt64();
  DbService::client()->execSqlSync(
      "INSERT INTO guard_camera_context (camera_id, role, environment_id) "
      "VALUES (11, 'other', " +
      std::to_string(doomed) + ")");
  DbService::client()->execSqlSync(
      "INSERT INTO guard_expected_guest (description, environment_id, "
      "valid_from, valid_until) VALUES ('Visitor', " +
      std::to_string(doomed) + ", 1, 2)");
  const int64_t before = changeRows();

  const Json::Value remaining = drogon::sync_wait(service.removeEnvironment(doomed));
  CHECK(changeRows() == before + 1);
  CHECK(lastPayload().find(idsOf({doomed})) != std::string::npos);
  CHECK(remaining.size() == 1);
  CHECK(remaining[0]["id"].asInt64() == kept);
  CHECK(scalarCount("SELECT COUNT(*) FROM guard_environment WHERE id = " +
                    std::to_string(doomed)) == 0);
  CHECK(scalarCount("SELECT COUNT(*) FROM guard_camera_context WHERE camera_id "
                    "= 11 AND environment_id = " +
                    std::to_string(kept)) == 1);
  CHECK(scalarCount("SELECT COUNT(*) FROM guard_expected_guest WHERE "
                    "environment_id = " +
                    std::to_string(doomed) +
                    " AND deleted_at IS NOT NULL") == 1);
  CHECK(postureRow(doomed) == 0);
}

TEST_CASE("placing a camera announces it, and a move announces both sides")
{
  sharedBoot();
  clearGuardTables();
  const auto bus = std::make_shared<NatsBus>();
  const ResourceChangeOutbox outbox(bus, ResourceChangeOutbox::Config{});
  const GuardFeatureService service(
      {.identity = nullptr, .disarm = nullptr, .changes = &outbox});

  const int64_t first = createEnvironment(service, "Place house")["id"].asInt64();
  const int64_t second = createEnvironment(service, "Place shop")["id"].asInt64();
  const int64_t before = changeRows();

  drogon::sync_wait(service.setCamera(
      {.cameraId = 21,
       .context = UpdateCameraContextDto{.role = "entrance",
                                         .outdoor = true,
                                         .publicArea = false,
                                         .activeHours = "",
                                         .environmentId = first}}));
  CHECK(changeRows() == before + 1);
  CHECK(lastPayload().find(idsOf({first})) != std::string::npos);

  drogon::sync_wait(service.setCamera(
      {.cameraId = 21,
       .context = UpdateCameraContextDto{.role = "entrance",
                                         .outdoor = true,
                                         .publicArea = false,
                                         .activeHours = "",
                                         .environmentId = second}}));
  CHECK(changeRows() == before + 2);
  CHECK(lastPayload().find(idsOf({second, first})) != std::string::npos);
  CHECK(scalarCount("SELECT environment_id FROM guard_camera_context WHERE "
                    "camera_id = 21") == second);

  drogon::sync_wait(service.setCamera(
      {.cameraId = 21,
       .context = UpdateCameraContextDto{.role = "perimeter",
                                         .outdoor = true,
                                         .publicArea = false,
                                         .activeHours = "",
                                         .environmentId = second}}));
  CHECK(changeRows() == before + 3);
  CHECK(lastPayload().find(idsOf({second})) != std::string::npos);
}

TEST_CASE("a camera without an environment lands in the default one")
{
  sharedBoot();
  clearGuardTables();
  const auto bus = std::make_shared<NatsBus>();
  const ResourceChangeOutbox outbox(bus, ResourceChangeOutbox::Config{});
  const GuardFeatureService service(
      {.identity = nullptr, .disarm = nullptr, .changes = &outbox});

  const int64_t fallback = insertDefaultEnvironment();
  const int64_t before = changeRows();

  drogon::sync_wait(service.setCamera(
      {.cameraId = 31,
       .context = UpdateCameraContextDto{.role = "other",
                                         .outdoor = false,
                                         .publicArea = false,
                                         .activeHours = "",
                                         .environmentId = std::nullopt}}));
  CHECK(changeRows() == before + 1);
  CHECK(lastPayload().find(idsOf({fallback})) != std::string::npos);
  CHECK(scalarCount("SELECT environment_id FROM guard_camera_context WHERE "
                    "camera_id = 31") == fallback);
}
