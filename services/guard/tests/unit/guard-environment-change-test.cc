#include <chrono>
#include <cstdio>
#include <doctest/doctest.h>
#include <drogon/drogon.h>
#include <feature/guard/dtos/create-environment-dto.hxx>
#include <feature/guard/dtos/create-expected-guest-dto.hxx>
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

#include "guard-fakes.hxx"

using guard_test::TempDb;
using guard_test::waitForBoot;
using guard_test::changeRows;
using guard_test::clearChangeOutbox;
using guard_test::idsOf;
using guard_test::lastPayload;
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
    drogon::app().addDbClient(drogon::orm::Sqlite3Config{
        .connectionNumber = 1, .filename = db.path(), .name = "default", .timeout = -1});
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
  clearChangeOutbox();
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

}

TEST_CASE("creating an environment announces it and stores its posture")
{
  sharedBoot();
  clearGuardTables();
  const auto bus = std::make_shared<NatsBus>();
  const ResourceChangeOutbox outbox(bus);
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
  const ResourceChangeOutbox outbox(bus);
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
  const ResourceChangeOutbox outbox(bus);
  const GuardFeatureService service(
      {.identity = nullptr, .disarm = nullptr, .changes = &outbox});

  const int64_t kept = insertDefaultEnvironment();
  const int64_t doomed = createEnvironment(service, "Doomed shop")["id"].asInt64();
  const int64_t empty = createEnvironment(service, "Empty shop")["id"].asInt64();
  drogon::sync_wait(service.setCamera(
      {.cameraId = 11,
       .context = UpdateCameraContextDto{.role = "other",
                                         .outdoor = false,
                                         .publicArea = false,
                                         .activeHours = "",
                                         .environmentId = doomed}}));
  const int64_t guestId = drogon::sync_wait(service.createGuest(
      {.body = CreateExpectedGuestDto{.description = "Visitor",
                                      .cameraId = 0,
                                      .environmentId = doomed,
                                      .personId = 0,
                                      .hostUserId = 0,
                                      .oneTime = false,
                                      .validFrom = 0,
                                      .validUntil = 0,
                                      .hours = 4},
       .callerId = 1,
       .callerRole = UserRole::Owner}));
  const int64_t before = changeRows();

  const Json::Value remaining = drogon::sync_wait(service.removeEnvironment(doomed));
  CHECK(changeRows() == before + 2);
  CHECK(lastPayload().find("\"scope\":\"expected_visits\"") != std::string::npos);
  CHECK(lastPayload().find(idsOf({guestId})) != std::string::npos);
  CHECK(remaining.size() == 2);
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

  const int64_t after = changeRows();
  drogon::sync_wait(service.removeEnvironment(empty));
  CHECK(changeRows() == after + 1);
  CHECK(lastPayload().find("\"scope\":\"environments\"") != std::string::npos);
}

TEST_CASE("an expected visit is announced when it is created and when it goes")
{
  sharedBoot();
  clearGuardTables();
  const auto bus = std::make_shared<NatsBus>();
  const ResourceChangeOutbox outbox(bus);
  const GuardFeatureService service(
      {.identity = nullptr, .disarm = nullptr, .changes = &outbox});
  const int64_t environmentId =
      createEnvironment(service, "Visit house")["id"].asInt64();
  const int64_t before = changeRows();

  const int64_t guestId = drogon::sync_wait(service.createGuest(
      {.body = CreateExpectedGuestDto{.description = "Ines",
                                      .cameraId = 0,
                                      .environmentId = environmentId,
                                      .personId = 0,
                                      .hostUserId = 0,
                                      .oneTime = true,
                                      .validFrom = 0,
                                      .validUntil = 0,
                                      .hours = 4},
       .callerId = 1,
       .callerRole = UserRole::Owner}));
  CHECK(guestId > 0);
  CHECK(changeRows() == before + 1);
  CHECK(lastPayload().find("\"domain\":\"guard\"") != std::string::npos);
  CHECK(lastPayload().find("\"scope\":\"expected_visits\"") != std::string::npos);
  CHECK(lastPayload().find(idsOf({guestId})) != std::string::npos);

  CHECK(drogon::sync_wait(service.removeGuest(guestId)));
  CHECK(changeRows() == before + 2);
  CHECK(lastPayload().find("\"scope\":\"expected_visits\"") != std::string::npos);
  CHECK(lastPayload().find(idsOf({guestId})) != std::string::npos);

  CHECK_FALSE(drogon::sync_wait(service.removeGuest(guestId)));
  CHECK(changeRows() == before + 2);
}

TEST_CASE("placing a camera announces it, and a move announces both sides")
{
  sharedBoot();
  clearGuardTables();
  const auto bus = std::make_shared<NatsBus>();
  const ResourceChangeOutbox outbox(bus);
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
  const ResourceChangeOutbox outbox(bus);
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
