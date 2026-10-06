#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <feature/module-data/services/productivity-module-data.hxx>
#include <sqlite/db-service.hxx>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

#ifndef ARGUS_PRODUCTIVITY_SCHEMA
#error "ARGUS_PRODUCTIVITY_SCHEMA must point at database/schema.sql"
#endif

namespace
{
constexpr const char* kModuleDataDb = "productivity-module-data-test.db";

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
  DbService::productivityClient()->execSqlSync(sql);
}

std::int64_t count(const std::string& table)
{
  const auto rows = DbService::productivityClient()->execSqlSync("SELECT COUNT(*) AS total FROM " + table);
  return rows.front()["total"].as<std::int64_t>();
}

std::int64_t itemCount(const ModuleDataSummary& summary, const std::string& kind)
{
  const auto found = std::ranges::find(summary.items, kind, &ModuleDataItem::kind);
  return found == summary.items.end() ? -1 : found->count;
}

void seed()
{
  exec("INSERT INTO project (id, owner_id, name, description) VALUES (1, 11, 'Huerto', 'Plantar'), "
       "(2, 11, 'Mudanza', '')");
  exec("UPDATE project SET deleted_at = 1 WHERE id = 2");
  exec("INSERT INTO project_task (project_id, title) VALUES (1, 'Comprar semillas'), (1, 'Regar')");
  exec("INSERT INTO project_member (project_id, user_id) VALUES (1, 12)");
  exec("INSERT INTO calendar_event (id, owner_id, title, starts_at) VALUES (1, 11, 'Cena', 1790000000)");
  exec("INSERT INTO calendar_event_share (calendar_event_id, user_id) VALUES (1, 12)");
  exec("INSERT INTO reminder (target_user_id, title, scheduled_at) VALUES (11, 'Pastilla', 1790000000)");
  exec("INSERT INTO agenda_notice (kind, ref_id, occurrence_at) VALUES ('event', 1, 1790000000), "
       "('reminder', 1, 1790000000)");
  exec("INSERT INTO idempotency_key (user_id, idem_key, route, record_id, created_at) VALUES "
       "(11, 'a', 'project', 1, 1), (11, 'b', 'reminder', 1, 1)");
  exec("INSERT INTO change_outbox (event_id, payload) VALUES ('productivity-1', '{}')");
}
}

TEST_CASE("the productivity data is summarized, purged in one transaction, and a retry is harmless")
{
  std::remove(kModuleDataDb);
  std::remove((std::string(kModuleDataDb) + "-wal").c_str());
  std::remove((std::string(kModuleDataDb) + "-shm").c_str());
  drogon::app().addDbClient(drogon::orm::Sqlite3Config{
      .connectionNumber = 1, .filename = kModuleDataDb, .name = "default", .timeout = -1});
  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));
  REQUIRE(DbService::runScriptFile(ARGUS_PRODUCTIVITY_SCHEMA, DbService::productivityClient()));
  seed();

  ProductivityModuleData data;

  const auto before = data.summary("productivity");
  CHECK(itemCount(before, "projects") == 1);
  CHECK(itemCount(before, "tasks") == 2);
  CHECK(itemCount(before, "project_members") == 1);
  CHECK(itemCount(before, "calendar_events") == 1);
  CHECK(itemCount(before, "calendar_shares") == 1);
  CHECK(before.bytes > 0);
  CHECK_FALSE(before.empty());

  CHECK(data.summary("surveillance").empty());
  const auto foreign = data.purge("surveillance");
  CHECK(foreign.purged);
  CHECK(count("project") == 2);

  exec("ALTER TABLE agenda_notice RENAME TO agenda_notice_away");
  CHECK_THROWS(data.purge("productivity"));
  CHECK(count("project") == 2);
  CHECK(count("calendar_event") == 1);
  exec("ALTER TABLE agenda_notice_away RENAME TO agenda_notice");

  const auto purged = data.purge("productivity");
  CHECK(purged.purged);
  CHECK(purged.reason.empty());
  for (const auto* table : {"project", "project_task", "project_member", "calendar_event", "calendar_event_share",
                            "change_outbox"})
    CHECK(count(table) == 0);
  CHECK(count("reminder") == 1);
  CHECK(count("agenda_notice") == 1);
  CHECK(count("idempotency_key") == 1);
  CHECK(data.summary("productivity").empty());

  CHECK(data.purge("productivity").purged);
  CHECK(data.summary("productivity").empty());
  CHECK(count("reminder") == 1);
}
