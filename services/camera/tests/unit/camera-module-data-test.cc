#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <feature/module-data/services/camera-module-data.hxx>
#include <feature/module-data/services/camera-module-impact.hxx>
#include <sqlite/db-service.hxx>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
constexpr const char* kModuleDataDb = "camera-module-data-test.db";

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
  DbService::cameraClient()->execSqlSync(sql);
}

std::int64_t count(const std::string& table)
{
  const auto rows = DbService::cameraClient()->execSqlSync("SELECT COUNT(*) AS total FROM " + table);
  return rows.front()["total"].as<std::int64_t>();
}

std::int64_t itemCount(const ModuleDataSummary& summary, const std::string& kind)
{
  const auto found = std::ranges::find(summary.items, kind, &ModuleDataItem::kind);
  return found == summary.items.end() ? -1 : found->count;
}

void seed()
{
  exec("INSERT INTO camera (id, name, ip, password) VALUES (1, 'Patio', '10.0.0.5', 'secret'), "
       "(2, 'Garaje', '10.0.0.6', 'secret')");
  exec("INSERT INTO camera_stream (camera_id, url) VALUES (1, 'rtsp://10.0.0.5/stream1')");
  exec("INSERT INTO zone (camera_id, name, points) VALUES (1, 'Puerta', '[[0,0],[1,1]]')");
  exec("INSERT INTO camera_evidence (camera_id, object_key, expires_at, deleted_at) VALUES "
       "(1, 'cameras/1/1_frame.jpg', 9999999999, 0), (1, 'cameras/1/1_person.jpg', 9999999999, 0), "
       "(1, 'cameras/1/0_frame.jpg', 1, 5)");
  exec("INSERT INTO action_command (command_id, kind, camera_id) VALUES ('c1', 'siren', 1)");
  exec("INSERT INTO siren_lease (camera_id, command_id, expires_at) VALUES (1, 'c1', 9999999999)");
  exec("INSERT INTO object_event_outbox (event_id, payload) VALUES ('e1', '{}')");
  exec("INSERT INTO camera_event_cooldown (camera_id, class) VALUES (1, 'person')");
  exec("INSERT INTO change_outbox (event_id, payload) VALUES ('camera-1', '{}')");
}

struct Recorder
{
  std::mutex mutex;
  std::vector<std::int64_t> forgotten;
  std::vector<std::string> removed;
  bool ready{false};
  bool failing{false};
};
}

TEST_CASE("argus-camera purges the surveillance data and its evidence objects, resumably")
{
  std::remove(kModuleDataDb);
  std::remove((std::string(kModuleDataDb) + "-wal").c_str());
  std::remove((std::string(kModuleDataDb) + "-shm").c_str());
  drogon::app().addDbClient(drogon::orm::Sqlite3Config{
      .connectionNumber = 1, .filename = kModuleDataDb, .name = "default", .timeout = -1});
  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));
  REQUIRE(DbService::runScriptFile(ARGUS_CAMERA_SCHEMA_PATH));
  seed();

  Recorder recorder;
  CameraModuleData data({.forgetCamera =
                             [&recorder](std::int64_t id) {
                               const std::scoped_lock lock(recorder.mutex);
                               recorder.forgotten.push_back(id);
                             },
                         .storageReady = [&recorder] { return recorder.ready; },
                         .removeObject = [&recorder](std::string key) -> drogon::Task<void> {
                           const std::scoped_lock lock(recorder.mutex);
                           if (recorder.failing)
                             throw std::runtime_error("object storage refused");
                           recorder.removed.push_back(std::move(key));
                           co_return;
                         },
                         .objectBudget = std::chrono::milliseconds(3000)});

  const auto before = data.summary("surveillance");
  CHECK(itemCount(before, "cameras") == 2);
  CHECK(itemCount(before, "zones") == 1);
  CHECK(itemCount(before, "evidence_photos") == 2);
  CHECK(itemCount(before, "camera_actions") == 1);
  CHECK(before.bytes > 0);
  CHECK(data.summary("productivity").empty());
  CHECK(data.purge("productivity").purged);
  CHECK(count("camera") == 2);

  exec("ALTER TABLE camera_event_cooldown RENAME TO camera_event_cooldown_away");
  CHECK_THROWS(data.purge("surveillance"));
  CHECK(count("camera") == 2);
  CHECK(count("zone") == 1);
  exec("ALTER TABLE camera_event_cooldown_away RENAME TO camera_event_cooldown");

  const auto withoutStorage = data.purge("surveillance");
  CHECK_FALSE(withoutStorage.purged);
  CHECK(withoutStorage.reason == "storage_unavailable");
  for (const auto* table : {"camera", "camera_stream", "zone", "action_command", "siren_lease", "object_event_outbox",
                            "camera_event_cooldown", "change_outbox"})
    CHECK(count(table) == 0);
  CHECK(count("camera_evidence") == 2);
  {
    const std::scoped_lock lock(recorder.mutex);
    std::ranges::sort(recorder.forgotten);
    CHECK(recorder.forgotten == std::vector<std::int64_t>{1, 2});
  }
  const auto pending = data.summary("surveillance");
  CHECK(itemCount(pending, "cameras") == 0);
  CHECK(itemCount(pending, "evidence_photos") == 2);

  recorder.ready = true;
  recorder.failing = true;
  const auto refused = data.purge("surveillance");
  CHECK_FALSE(refused.purged);
  CHECK(refused.reason == "storage_failed");
  CHECK(count("camera_evidence") == 2);

  recorder.failing = false;
  const auto done = data.purge("surveillance");
  CHECK(done.purged);
  CHECK(count("camera_evidence") == 0);
  {
    const std::scoped_lock lock(recorder.mutex);
    std::ranges::sort(recorder.removed);
    CHECK(recorder.removed == std::vector<std::string>{"cameras/1/1_frame.jpg", "cameras/1/1_person.jpg"});
  }
  CHECK(data.summary("surveillance").empty());
  CHECK(data.purge("surveillance").purged);
}

TEST_CASE("the camera reports its open talk sessions and live views as what surveillance stops, and nothing for another module")
{
  std::size_t talks = 2;
  std::size_t views = 5;
  const CameraModuleImpact host({.talkSessions = [&talks] { return talks; }, .liveViews = [&views] { return views; }});

  const auto report = host.impact("surveillance");
  REQUIRE(report.stops.size() == 2);
  CHECK(report.stops[0].kind == "camera_talk");
  CHECK(report.stops[0].count == 2);
  CHECK(report.stops[1].kind == "live_views");
  CHECK(report.stops[1].count == 5);

  talks = 0;
  views = 0;
  CHECK(host.impact("surveillance").stops[0].count == 0);
  CHECK(host.impact("productivity").stops.empty());
  CHECK(host.impact("surveillance").roleHolders.empty());
}
