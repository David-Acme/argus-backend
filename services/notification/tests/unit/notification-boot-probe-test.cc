#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <test-support/app-runner.hxx>
#include <shared/services/notification/notification-service.hxx>
#include <sqlite/db-service.hxx>

#include <chrono>
#include <cstdio>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>

#ifndef ARGUS_NOTIFICATION_SCHEMA
#error "ARGUS_NOTIFICATION_SCHEMA must point at database/schema.sql"
#endif

namespace
{
using test_support::AppRunner;

struct BootDb
{
  std::string path{"notification-boot-probe-test-" + std::to_string(::getpid()) + ".db"};
  std::optional<AppRunner> runner;

  BootDb()
  {
    drogon::app().setLogLevel(trantor::Logger::kWarn);
    drogon::app().addDbClient(drogon::orm::Sqlite3Config{.connectionNumber = 1,
                                                          .filename = path,
                                                          .name = "default",
                                                          .timeout = -1});
    runner.emplace();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (!drogon::app().isRunning() && std::chrono::steady_clock::now() < deadline)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (!drogon::app().isRunning())
      throw std::runtime_error("drogon loop did not boot");
  }

  ~BootDb()
  {
    runner.reset();
    std::remove(path.c_str());
    std::remove((path + "-wal").c_str());
    std::remove((path + "-shm").c_str());
  }
};

BootDb& bootDb()
{
  static BootDb db;
  return db;
}
}

TEST_CASE("the boot probe fails loudly on a database without its schema and passes once the schema is applied")
{
  BootDb& db = bootDb();
  (void)db;
  const NotificationService service;

  CHECK_THROWS(drogon::sync_wait(service.runProbe()));

  REQUIRE(DbService::runScriptFile(ARGUS_NOTIFICATION_SCHEMA));
  const auto state = drogon::sync_wait(service.runProbe());
  CHECK(state.at > 0);
}
