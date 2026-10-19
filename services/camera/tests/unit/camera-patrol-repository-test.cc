#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <feature/camera-control/repositories/camera-patrol/camera-patrol-repository.hxx>
#include <sqlite/db-service.hxx>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

namespace
{
constexpr const char* kPatrolDb = "camera-patrol-repository-test.db";

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

std::int64_t patrolRows()
{
  const auto rows = DbService::cameraClient()->execSqlSync("SELECT COUNT(*) AS total FROM camera_patrol");
  return rows.front()["total"].as<std::int64_t>();
}
}

TEST_CASE("a patrol is stored per camera, patched field by field and gone with its camera")
{
  std::remove(kPatrolDb);
  std::remove((std::string(kPatrolDb) + "-wal").c_str());
  std::remove((std::string(kPatrolDb) + "-shm").c_str());
  drogon::app().addDbClient(drogon::orm::Sqlite3Config{
      .connectionNumber = 1, .filename = kPatrolDb, .name = "default", .timeout = -1});
  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));
  REQUIRE(DbService::runScriptFile(ARGUS_CAMERA_SCHEMA_PATH));
  exec("PRAGMA foreign_keys = ON");
  exec("INSERT INTO camera (id, name, ip, is_online) VALUES (1, 'Patio', '10.0.0.5', 1), "
       "(2, 'Garaje', '10.0.0.6', 0), (3, 'Sala', '10.0.0.7', 1)");
  exec("UPDATE camera SET is_enabled = 0 WHERE id = 3");

  const CameraPatrolRepository repository{};
  CHECK_FALSE(drogon::sync_wait(repository.findByCamera(1)).has_value());

  const auto created = drogon::sync_wait(repository.create({.cameraId = 1,
                                                            .stops = {"1", "2", "3"},
                                                            .dwellSeconds = 20,
                                                            .isEnabled = true,
                                                            .window = PatrolWindow{.startHour = 22, .endHour = 6},
                                                            .client = nullptr}));
  CHECK(created.cameraId == 1);
  CHECK(created.stops == std::vector<std::string>{"1", "2", "3"});

  const auto stored = drogon::sync_wait(repository.findByCamera(1));
  REQUIRE(stored.has_value());
  CHECK(stored->stops == std::vector<std::string>{"1", "2", "3"});
  CHECK(stored->dwellSeconds == 20);
  CHECK(stored->isEnabled);
  REQUIRE(stored->window.has_value());
  CHECK(stored->window->startHour == 22);
  CHECK(stored->window->endHour == 6);
  CHECK_FALSE(stored->updatedAt.has_value());
  CHECK(stored->toJson()["window"]["endHour"].asInt() == 6);

  const auto dwell = drogon::sync_wait(repository.update(1,
                                                         {.stops = std::nullopt,
                                                          .dwellSeconds = 45,
                                                          .isEnabled = std::nullopt,
                                                          .window = std::nullopt,
                                                          .client = nullptr}));
  REQUIRE(dwell.has_value());
  CHECK(dwell->dwellSeconds == 45);
  CHECK(dwell->stops.size() == 3);
  CHECK(dwell->window.has_value());
  CHECK(dwell->updatedAt.has_value());

  const auto cleared = drogon::sync_wait(repository.update(
      1,
      {.stops = std::vector<std::string>{"4", "5"},
       .dwellSeconds = std::nullopt,
       .isEnabled = false,
       .window = std::optional<std::optional<PatrolWindow>>(std::optional<PatrolWindow>()),
       .client = nullptr}));
  REQUIRE(cleared.has_value());
  CHECK(cleared->stops == std::vector<std::string>{"4", "5"});
  CHECK_FALSE(cleared->isEnabled);
  CHECK_FALSE(cleared->window.has_value());
  CHECK(cleared->toJson()["window"].isNull());

  const auto untouched = drogon::sync_wait(repository.update(1,
                                                             {.stops = std::nullopt,
                                                              .dwellSeconds = std::nullopt,
                                                              .isEnabled = std::nullopt,
                                                              .window = std::nullopt,
                                                              .client = nullptr}));
  REQUIRE(untouched.has_value());
  CHECK(untouched->dwellSeconds == 45);
  CHECK_FALSE(drogon::sync_wait(repository.update(9,
                                                  {.stops = std::nullopt,
                                                   .dwellSeconds = 30,
                                                   .isEnabled = std::nullopt,
                                                   .window = std::nullopt,
                                                   .client = nullptr}))
                    .has_value());

  for (const std::int64_t camera : {2, 3})
    static_cast<void>(drogon::sync_wait(repository.create({.cameraId = camera,
                                                           .stops = {"a", "b"},
                                                           .dwellSeconds = 30,
                                                           .isEnabled = true,
                                                           .window = std::nullopt,
                                                           .client = nullptr})));
  const auto active = drogon::sync_wait(repository.findActive());
  REQUIRE(active.size() == 1);
  CHECK(active.front().patrol.cameraId == 2);
  CHECK_FALSE(active.front().cameraOnline);

  static_cast<void>(drogon::sync_wait(repository.update(1,
                                                        {.stops = std::nullopt,
                                                         .dwellSeconds = std::nullopt,
                                                         .isEnabled = true,
                                                         .window = std::nullopt,
                                                         .client = nullptr})));
  const auto both = drogon::sync_wait(repository.findActive());
  REQUIRE(both.size() == 2);
  CHECK(both.front().patrol.cameraId == 1);
  CHECK(both.front().cameraOnline);

  CHECK_THROWS(exec("UPDATE camera_patrol SET dwell_seconds = 4 WHERE camera_id = 1"));
  CHECK_THROWS(exec("UPDATE camera_patrol SET start_hour = 3 WHERE camera_id = 1"));

  CHECK(drogon::sync_wait(repository.remove(1)));
  CHECK_FALSE(drogon::sync_wait(repository.remove(1)));
  CHECK_FALSE(drogon::sync_wait(repository.findByCamera(1)).has_value());

  CHECK(patrolRows() == 2);
  exec("DELETE FROM camera WHERE id = 2");
  CHECK(patrolRows() == 1);
}
