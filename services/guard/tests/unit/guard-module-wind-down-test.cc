#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <chrono>
#include <drogon/drogon.h>
#include <feature/guard/services/guard-module-wind-down.hxx>
#include <sqlite/db-service.hxx>
#include <string>
#include <thread>

#include "temp-db.hxx"
#include "wait-for-boot.hxx"

using guard_test::TempDb;
using guard_test::waitForBoot;

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

void seedObservation(const std::string& eventId, const std::string& status)
{
  DbService::client()->execSqlSync(
      "INSERT INTO guard_observation_inbox (event_id, camera_id, status, retry_at, payload) "
      "VALUES (?, 1, ?, 100, '{}')",
      eventId, status);
}

void seedAction(const std::string& commandId, const std::string& status)
{
  DbService::client()->execSqlSync(
      "INSERT INTO guard_action_outbox (command_id, kind, status) VALUES (?, 'notify', ?)", commandId, status);
}

void seedRecipient(int64_t environmentId, int64_t userId, int onDuty)
{
  DbService::client()->execSqlSync(
      "INSERT INTO guard_response_recipient (environment_id, user_id, on_duty, updated_at) VALUES (?, ?, ?, 1)",
      environmentId, userId, onDuty);
}

void seedSafetyAlert(const std::string& kind, int64_t userId)
{
  DbService::client()->execSqlSync(
      "INSERT INTO guard_safety_alert (kind, user_id, created_at) VALUES (?, ?, 1)", kind, userId);
}
}

TEST_CASE("turning surveillance off drops pending work and duty, and leaves safety alerts and settled work alone")
{
  const TempDb db("guard-module-wind-down-test");
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{.connectionNumber = 1, .filename = db.path(), .name = "default", .timeout = -1});
  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));
  REQUIRE(DbService::runScriptFile(ARGUS_GUARD_SCHEMA_PATH));

  const GuardModuleWindDown windDown;
  const auto idle = drogon::sync_wait(windDown.run());
  CHECK(idle.empty());

  seedObservation("event:waiting", "processing");
  seedObservation("event:retrying", "processing");
  seedObservation("event:done", "completed");
  seedObservation("event:parked", "dead_lettered");
  seedAction("cmd:pending", "pending");
  seedAction("cmd:in-flight", "in_flight");
  seedAction("cmd:retry", "retryable_failed");
  seedAction("cmd:sent", "succeeded");
  seedAction("cmd:refused", "rejected");
  seedRecipient(1, 10, 1);
  seedRecipient(1, 11, 0);
  seedRecipient(2, 12, 1);
  seedSafetyAlert("panic", 10);
  seedSafetyAlert("duress", 11);

  const auto report = drogon::sync_wait(windDown.run());
  CHECK(report.observations == 2);
  CHECK(report.actions == 3);
  CHECK(report.duty == 2);
  CHECK_FALSE(report.empty());

  CHECK(scalar("SELECT COUNT(*) FROM guard_observation_inbox WHERE status = 'processing'") == "0");
  CHECK(scalar("SELECT status FROM guard_observation_inbox WHERE event_id = 'event:waiting'") == "completed");
  CHECK(scalar("SELECT status FROM guard_observation_inbox WHERE event_id = 'event:parked'") == "dead_lettered");
  CHECK(scalar("SELECT status FROM guard_action_outbox WHERE command_id = 'cmd:pending'") == "rejected");
  CHECK(scalar("SELECT status FROM guard_action_outbox WHERE command_id = 'cmd:in-flight'") == "rejected");
  CHECK(scalar("SELECT status FROM guard_action_outbox WHERE command_id = 'cmd:retry'") == "rejected");
  CHECK(scalar("SELECT detail FROM guard_action_outbox WHERE command_id = 'cmd:retry'") == "module_disabled");
  CHECK(scalar("SELECT status FROM guard_action_outbox WHERE command_id = 'cmd:sent'") == "succeeded");
  CHECK(scalar("SELECT COUNT(*) FROM guard_response_recipient WHERE on_duty = 1") == "0");
  CHECK(scalar("SELECT COUNT(*) FROM guard_response_recipient") == "3");
  CHECK(scalar("SELECT COUNT(*) FROM guard_safety_alert") == "2");

  const auto again = drogon::sync_wait(windDown.run());
  CHECK(again.empty());
}
