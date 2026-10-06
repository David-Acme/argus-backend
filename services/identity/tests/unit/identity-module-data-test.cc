#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <feature/module-data/services/identity-module-data.hxx>
#include <sqlite/db-service.hxx>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#ifndef ARGUS_IDENTITY_SCHEMA
#error "ARGUS_IDENTITY_SCHEMA must point at database/schema.sql"
#endif

namespace
{
constexpr const char* kModuleDataDb = "identity-module-data-test.db";

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
  DbService::identityClient()->execSqlSync(sql);
}

std::string scalar(const std::string& sql)
{
  const auto rows = DbService::identityClient()->execSqlSync(sql);
  return rows.empty() ? std::string{} : rows.front()[0].as<std::string>();
}

std::int64_t itemCount(const ModuleDataSummary& summary, const std::string& kind)
{
  const auto found = std::ranges::find(summary.items, kind, &ModuleDataItem::kind);
  return found == summary.items.end() ? -1 : found->count;
}

void seed()
{
  exec("INSERT INTO user (id, name, last_name, role) VALUES (1, 'Ana', 'Ruiz', 'owner')");
  exec("INSERT INTO person (id, user_id, name) VALUES (1, 1, 'Ana')");
  exec("INSERT INTO person (id, user_id, name, status, visitor_number) VALUES "
       "(2, NULL, 'Gasfitero', 'known', 1), (3, NULL, '', 'candidate', 2), (4, NULL, '', 'candidate', 3)");
  exec("UPDATE person SET deleted_at = 1 WHERE id = 4");
  exec("UPDATE visitor_counter SET last_number = 3 WHERE id = 1");
  exec("INSERT INTO face_embedding (id, person_id, embedding, crop_key) VALUES "
       "(10, 1, x'00', ''), (11, 2, x'0102', 'visitors/2/a.jpg'), (12, 3, x'0304', 'visitors/3/b.jpg'), "
       "(13, 3, x'05', '')");
  exec("INSERT INTO person_visit (person_id, camera_id, started_at, last_seen_at) VALUES (2, 1, 1, 2), (3, 1, 3, 4)");
  exec("INSERT INTO person_snapshot (person_id, image) VALUES (1, x'ffd8'), (2, x'ffd9')");
  exec("INSERT INTO person_tag (person_id, tag) VALUES (2, 'casco')");
}
}

TEST_CASE("argus-identity purges every visitor with their samples, visits and crops, and keeps the household")
{
  std::remove(kModuleDataDb);
  std::remove((std::string(kModuleDataDb) + "-wal").c_str());
  std::remove((std::string(kModuleDataDb) + "-shm").c_str());
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(drogon::orm::Sqlite3Config{
      .connectionNumber = 1, .filename = kModuleDataDb, .name = "default", .timeout = -1});
  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));
  REQUIRE(DbService::runScriptFile(ARGUS_IDENTITY_SCHEMA));
  seed();

  std::mutex mutex;
  std::vector<std::int64_t> forgotten;
  std::atomic<int> kicks{0};
  IdentityModuleData data({.forgetVectors =
                               [&](const std::vector<std::int64_t>& ids) {
                                 const std::scoped_lock lock(mutex);
                                 forgotten.insert(forgotten.end(), ids.begin(), ids.end());
                               },
                           .kickDeletion = [&kicks] { kicks.fetch_add(1); }});

  const auto before = data.summary("surveillance");
  CHECK(itemCount(before, "visitors") == 2);
  CHECK(itemCount(before, "visitor_face_samples") == 3);
  CHECK(itemCount(before, "visits") == 2);
  CHECK(before.bytes > 0);
  CHECK(data.summary("productivity").empty());
  CHECK(data.purge("productivity").purged);

  exec("ALTER TABLE visitor_counter RENAME TO visitor_counter_away");
  CHECK_THROWS(data.purge("surveillance"));
  CHECK(scalar("SELECT COUNT(*) FROM person") == "4");
  CHECK(scalar("SELECT COUNT(*) FROM face_embedding") == "4");
  CHECK(scalar("SELECT COUNT(*) FROM pending_object_delete") == "0");
  exec("ALTER TABLE visitor_counter_away RENAME TO visitor_counter");

  const auto purged = data.purge("surveillance");
  CHECK(purged.purged);
  CHECK(scalar("SELECT COUNT(*) FROM person") == "1");
  CHECK(scalar("SELECT user_id FROM person") == "1");
  CHECK(scalar("SELECT COUNT(*) FROM face_embedding") == "1");
  CHECK(scalar("SELECT id FROM face_embedding") == "10");
  CHECK(scalar("SELECT COUNT(*) FROM person_visit") == "0");
  CHECK(scalar("SELECT COUNT(*) FROM person_snapshot") == "1");
  CHECK(scalar("SELECT COUNT(*) FROM person_tag") == "0");
  CHECK(scalar("SELECT last_number FROM visitor_counter") == "0");
  CHECK(scalar("SELECT group_concat(object_key, ',') FROM (SELECT object_key FROM pending_object_delete "
               "ORDER BY object_key)") == "visitors/2/a.jpg,visitors/3/b.jpg");
  {
    const std::scoped_lock lock(mutex);
    std::ranges::sort(forgotten);
    CHECK(forgotten == std::vector<std::int64_t>{11, 12, 13});
  }
  CHECK(kicks.load() == 1);
  CHECK(data.summary("surveillance").empty());

  CHECK(data.purge("surveillance").purged);
  CHECK(kicks.load() == 1);
  CHECK(scalar("SELECT COUNT(*) FROM person") == "1");
}
