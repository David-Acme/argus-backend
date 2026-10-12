#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <chrono>
#include <cstdio>
#include <ctime>
#include <doctest/doctest.h>
#include <drogon/drogon.h>
#include <feature/guard/dtos/update-guard-mode-dto.hxx>
#include <feature/guard/infra/resource-change-outbox.hxx>
#include <feature/guard/services/guard-feature-service.hxx>
#include <feature/guard/services/posture-watch.hxx>
#include <memory>
#include <nats/nats-bus.hxx>
#include <optional>
#include <sqlite/db-service.hxx>
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
  TempDb db{"guard-posture-watch-test"};
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

int64_t insertEnvironment(const std::string& name, const std::string& mode)
{
  DbService::client()->execSqlSync(
      "INSERT INTO guard_environment (name, kind, is_default, mode, "
      "schedule_enabled, asleep_hours, closed_mode) VALUES ('" +
      name + "', 'home', 0, '" + mode + "', 1, '22:00-06:00', 'away')");
  return scalarCount("SELECT id FROM guard_environment WHERE name = '" + name +
                     "'");
}

int64_t insertUnscheduledEnvironment(const std::string& name,
                                     const std::string& mode)
{
  DbService::client()->execSqlSync(
      "INSERT INTO guard_environment (name, kind, is_default, mode, "
      "schedule_enabled, asleep_hours, closed_mode) VALUES ('" +
      name + "', 'home', 0, '" + mode + "', 0, '', 'away')");
  return scalarCount("SELECT id FROM guard_environment WHERE name = '" + name +
                     "'");
}

void clearGuardTables()
{
  DbService::client()->execSqlSync("DELETE FROM change_outbox");
  DbService::client()->execSqlSync("DELETE FROM guard_state");
  DbService::client()->execSqlSync("DELETE FROM guard_environment");
}

std::tm localAt(int hour, int minute)
{
  std::tm local{};
  local.tm_year = 126;
  local.tm_mon = 5;
  local.tm_mday = 2;
  local.tm_wday = 2;
  local.tm_hour = hour;
  local.tm_min = minute;
  return local;
}

int64_t changeRows()
{
  return scalarCount("SELECT COUNT(*) FROM change_outbox");
}
}

TEST_CASE("a scheduled transition is announced without touching the manual mode")
{
  sharedBoot();
  const int64_t environmentId = insertEnvironment("Posture house", "home");
  const auto bus = std::make_shared<NatsBus>();
  const ResourceChangeOutbox outbox(bus, ResourceChangeOutbox::Config{});
  int64_t at = 1780000000;
  std::tm local = localAt(23, 0);
  const PostureWatch watch(
      {.changes = &outbox},
      {.intervalSeconds = 30,
       .clock = {.now = [&at]() { return at; }, .local = [&local]() {
                   return local;
                 }}});

  const int64_t before = changeRows();
  drogon::sync_wait(watch.evaluate());
  CHECK(changeRows() == before + 1);
  CHECK(scalarText("SELECT mode FROM guard_environment WHERE id = " +
                   std::to_string(environmentId)) == "home");

  drogon::sync_wait(watch.evaluate());
  CHECK(changeRows() == before + 1);

  at += 3600;
  local = localAt(10, 0);
  drogon::sync_wait(watch.evaluate());
  CHECK(changeRows() == before + 2);
  CHECK(scalarText("SELECT mode FROM guard_environment WHERE id = " +
                   std::to_string(environmentId)) == "home");
  CHECK(scalarText("SELECT payload FROM change_outbox ORDER BY rowid DESC "
                   "LIMIT 1")
            .find("\"scope\":\"environments\"") != std::string::npos);
}

TEST_CASE("a mode change announces every targeted environment in its own row")
{
  sharedBoot();
  const auto bus = std::make_shared<NatsBus>();
  const ResourceChangeOutbox outbox(bus, ResourceChangeOutbox::Config{});
  const GuardFeatureService service(
      {.identity = nullptr, .disarm = nullptr, .changes = &outbox});
  const int64_t first = insertEnvironment("Mode house", "away");
  const int64_t second = insertEnvironment("Mode shop", "away");

  const int64_t before = changeRows();
  drogon::sync_wait(service.setMode(
      {.body = UpdateGuardModeDto{.mode = "home",
                                  .environmentId = first,
                                  .pin = std::nullopt},
       .userId = 7,
       .userName = "Ana"}));
  CHECK(changeRows() == before + 1);
  CHECK(scalarText("SELECT mode FROM guard_environment WHERE id = " +
                   std::to_string(first)) == "home");

  drogon::sync_wait(service.setMode(
      {.body = UpdateGuardModeDto{.mode = "home",
                                  .environmentId = second,
                                  .pin = std::nullopt},
       .userId = 7,
       .userName = "Ana"}));
  CHECK(changeRows() == before + 2);

  const int64_t environments = scalarCount(
      "SELECT COUNT(*) FROM guard_environment WHERE schedule_enabled = 1");
  drogon::sync_wait(service.setMode(
      {.body = UpdateGuardModeDto{.mode = "away",
                                  .environmentId = std::nullopt,
                                  .pin = std::nullopt},
       .userId = 7,
       .userName = "Ana"}));
  CHECK(changeRows() == before + 2 + environments);
}

TEST_CASE("the first evaluation after a restart reconciles, and only when the posture moved")
{
  sharedBoot();
  clearGuardTables();
  const int64_t asleep = insertEnvironment("Recon house", "home");
  insertUnscheduledEnvironment("Recon shop", "home");

  const auto bus = std::make_shared<NatsBus>();
  const ResourceChangeOutbox outbox(bus, ResourceChangeOutbox::Config{});
  int64_t at = 1780000000;
  std::tm local = localAt(23, 0);
  const PostureWatch watch(
      {.changes = &outbox},
      {.intervalSeconds = 30,
       .clock = {.now = [&at]() { return at; }, .local = [&local]() {
                   return local;
                 }}});

  drogon::sync_wait(watch.evaluate());
  CHECK(changeRows() == 2);
  CHECK_FALSE(scalarText("SELECT value FROM guard_state WHERE key = '" +
                         guard_schedule::stateKey(asleep) + "'")
                  .empty());

  drogon::sync_wait(watch.evaluate());
  CHECK(changeRows() == 2);

  DbService::client()->execSqlSync(
      "UPDATE guard_state SET value = 'home|manual|0|0' WHERE key = '" +
      guard_schedule::stateKey(asleep) + "'");
  drogon::sync_wait(watch.evaluate());
  CHECK(changeRows() == 3);
  CHECK(scalarText("SELECT value FROM guard_state WHERE key = '" +
                   guard_schedule::stateKey(asleep) + "'") !=
        "home|manual|0|0");
}

TEST_CASE("a manual change followed by a tick announces exactly once")
{
  sharedBoot();
  clearGuardTables();
  const int64_t environmentId =
      insertUnscheduledEnvironment("Manual shop", "away");
  const auto bus = std::make_shared<NatsBus>();
  const ResourceChangeOutbox outbox(bus, ResourceChangeOutbox::Config{});
  const GuardFeatureService service(
      {.identity = nullptr, .disarm = nullptr, .changes = &outbox});
  const PostureWatch watch({.changes = &outbox}, {.intervalSeconds = 30});

  drogon::sync_wait(service.setMode(
      {.body = UpdateGuardModeDto{.mode = "home",
                                  .environmentId = environmentId,
                                  .pin = std::nullopt},
       .userId = 7,
       .userName = "Ana"}));
  CHECK(changeRows() == 1);

  drogon::sync_wait(watch.evaluate());
  CHECK(changeRows() == 1);
}
