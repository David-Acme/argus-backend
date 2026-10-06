#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <feature/module-data/services/sync-module-data.hxx>
#include <sqlite/db-service.hxx>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <unistd.h>

#ifndef ARGUS_SYNC_SCHEMA_PATH
#error "ARGUS_SYNC_SCHEMA_PATH must point at the sync schema.sql"
#endif

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

bool waitForBoot(std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (drogon::app().isRunning())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return drogon::app().isRunning();
}

void exec(const std::string& sql)
{
  DbService::client()->execSqlSync(sql);
}

std::int64_t rowsOf(const std::string& table, const std::string& name)
{
  const auto rows =
      DbService::client()->execSqlSync("SELECT COUNT(*) AS total FROM " + table + " WHERE table_name = ?", name);
  return rows.front()["total"].as<std::int64_t>();
}

std::int64_t history(const ModuleDataSummary& summary)
{
  return summary.items.empty() ? -1 : summary.items.front().count;
}

void seed()
{
  for (const auto* table : {"camera", "zone", "project", "calendar_event", "reminder"}) {
    const std::string name(table);
    exec("INSERT INTO audit_log (record_id, table_name, changes, event_timestamp) VALUES "
         "(1, '" + name + R"(', '{"name":["Old","New"]}', 1))");
    exec("INSERT INTO user_audit_log (user_id, record_id, table_name, changes, event_timestamp) VALUES "
         "(11, 1, '" + name + R"(', '{"title":["a","b"]}', 1))");
    exec("INSERT INTO user_action_log (user_id, record_id, table_name, action, old_data) VALUES "
         "(11, 1, '" + name + R"(', 'update', '{"name":"Old"}'))");
    exec("INSERT INTO audit_compaction_state (table_name, compacted_through_id) VALUES ('" + name + "', 1)");
  }
}
}

TEST_CASE("sync purges the change history of a module's tables only, in one transaction, idempotently")
{
  const std::string path = "sync-module-data-test-" + std::to_string(::getpid()) + ".db";
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{.connectionNumber = 1, .filename = path, .name = "default", .timeout = -1});
  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));
  REQUIRE(DbService::runScriptFile(ARGUS_SYNC_SCHEMA_PATH));
  seed();

  SyncModuleData data;
  CHECK(history(data.summary("surveillance")) == 6);
  CHECK(history(data.summary("productivity")) == 6);
  CHECK(data.summary("productivity").bytes > 0);
  CHECK(data.summary("core").empty());
  CHECK(data.purge("core").purged);

  exec("ALTER TABLE audit_compaction_state RENAME TO audit_compaction_state_away");
  CHECK_THROWS(data.purge("productivity"));
  CHECK(rowsOf("audit_log", "project") == 1);
  CHECK(rowsOf("user_audit_log", "calendar_event") == 1);
  exec("ALTER TABLE audit_compaction_state_away RENAME TO audit_compaction_state");

  const auto purged = data.purge("productivity");
  CHECK(purged.purged);
  for (const auto* table : {"audit_log", "user_audit_log", "user_action_log", "audit_compaction_state"}) {
    CHECK(rowsOf(table, "project") == 0);
    CHECK(rowsOf(table, "calendar_event") == 0);
    CHECK(rowsOf(table, "camera") == 1);
    CHECK(rowsOf(table, "reminder") == 1);
  }
  CHECK(data.summary("productivity").empty());
  CHECK(history(data.summary("surveillance")) == 6);

  CHECK(data.purge("surveillance").purged);
  CHECK(data.summary("surveillance").empty());
  CHECK(rowsOf("audit_log", "reminder") == 1);
  CHECK(data.purge("surveillance").purged);

  std::remove(path.c_str());
  std::remove((path + "-wal").c_str());
  std::remove((path + "-shm").c_str());
}
