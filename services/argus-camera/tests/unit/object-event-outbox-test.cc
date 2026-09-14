#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <shared/repositories/object-event-outbox/object-event-outbox-repository.hxx>
#include <shared/services/sqlite/db-service.hxx>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>

namespace
{
constexpr const char* kOutboxDb = "camera-outbox-test.db";

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
} // namespace

TEST_CASE("the object event outbox commits, dedups and survives restarts")
{
  std::remove(kOutboxDb);
  std::remove((std::string(kOutboxDb) + "-wal").c_str());
  std::remove((std::string(kOutboxDb) + "-shm").c_str());
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{1, kOutboxDb, "default", -1});
  std::thread runner([] { drogon::app().run(); });
  REQUIRE(waitForBoot(std::chrono::seconds(30)));
  REQUIRE(DbService::runScriptFile(ARGUS_CAMERA_SCHEMA_PATH));

  ObjectEventOutboxRepository repository;

  const ObjectEventEnqueueInput first = {.eventId = "cam:1",
                                         .payload = "{\"a\":1}",
                                         .cameraId = 1,
                                         .cooldownClasses = {"person"},
                                         .nowMs = 1000,
                                         .cooldownMs = 30000,
                                         .maxPending = 10};
  CHECK(repository.enqueue(first).result ==
        ObjectEventEnqueueResult::Recorded);

  CHECK(repository.enqueue({.eventId = "cam:2",
                            .payload = "{\"a\":2}",
                            .cameraId = 1,
                            .cooldownClasses = {"person"},
                            .nowMs = 2000,
                            .cooldownMs = 30000,
                            .maxPending = 10}).result ==
        ObjectEventEnqueueResult::Suppressed);

  CHECK(repository.enqueue({.eventId = "cam:3",
                            .payload = "{\"a\":3}",
                            .cameraId = 1,
                            .cooldownClasses = {"person"},
                            .nowMs = 40000,
                            .cooldownMs = 30000,
                            .maxPending = 10}).result ==
        ObjectEventEnqueueResult::Recorded);

  const auto pending = repository.nextPending();
  REQUIRE(pending.has_value());
  CHECK(pending->eventId == "cam:1");
  CHECK(repository.markSent("cam:1", 50000));
  CHECK_FALSE(repository.markSent("cam:1", 50001));
  CHECK(repository.recordAttempt("cam:3"));

  const ObjectEventOutboxStats stats = repository.stats();
  CHECK(stats.sent == 1);
  CHECK(stats.pending == 1);

  CHECK(repository.enqueue({.eventId = "cam:4",
                            .payload = "{\"a\":4}",
                            .cameraId = 2,
                            .cooldownClasses = {},
                            .nowMs = 60000,
                            .cooldownMs = 0,
                            .maxPending = 1}).result ==
        ObjectEventEnqueueResult::Recorded);
  CHECK(repository.enqueue({.eventId = "cam:5",
                            .payload = "{\"a\":5}",
                            .cameraId = 2,
                            .cooldownClasses = {},
                            .nowMs = 60001,
                            .cooldownMs = 0,
                            .maxPending = 1}).result ==
        ObjectEventEnqueueResult::Recorded);
  CHECK(repository.stats().overflowDropped == 2);

  CHECK(repository.enqueue({.eventId = "p:session-1",
                            .payload = "{}",
                            .cameraId = 3,
                            .cooldownClasses = {"person:S1:1"},
                            .nowMs = 100000,
                            .cooldownMs = 30000,
                            .maxPending = 100})
            .result == ObjectEventEnqueueResult::Recorded);
  CHECK(repository.enqueue({.eventId = "p:session-1-again",
                            .payload = "{}",
                            .cameraId = 3,
                            .cooldownClasses = {"person:S1:1"},
                            .nowMs = 110000,
                            .cooldownMs = 30000,
                            .maxPending = 100})
            .result == ObjectEventEnqueueResult::Suppressed);
  CHECK(repository.enqueue({.eventId = "p:session-2",
                            .payload = "{}",
                            .cameraId = 3,
                            .cooldownClasses = {"person:S2:1"},
                            .nowMs = 110000,
                            .cooldownMs = 30000,
                            .maxPending = 100})
            .result == ObjectEventEnqueueResult::Recorded);

  const int64_t purged = repository.purgeExpiredCooldowns(105000);
  CHECK(purged >= 1);
  CHECK(repository.enqueue({.eventId = "p:session-2-again",
                            .payload = "{}",
                            .cameraId = 3,
                            .cooldownClasses = {"person:S2:1"},
                            .nowMs = 120000,
                            .cooldownMs = 30000,
                            .maxPending = 100})
            .result == ObjectEventEnqueueResult::Suppressed);

  CHECK(repository.enqueue({.eventId = "dup:1",
                            .payload = "{}",
                            .cameraId = 4,
                            .cooldownClasses = {"k"},
                            .nowMs = 1000,
                            .cooldownMs = 5000,
                            .maxPending = 100})
            .result == ObjectEventEnqueueResult::Recorded);
  const ObjectEventEnqueueOutcome duplicate = repository.enqueue(
      {.eventId = "dup:1",
       .payload = "{}",
       .cameraId = 4,
       .cooldownClasses = {"k"},
       .nowMs = 6500,
       .cooldownMs = 5000,
       .maxPending = 100});
  CHECK(duplicate.result == ObjectEventEnqueueResult::Recorded);
  CHECK_FALSE(duplicate.inserted);
  CHECK(duplicate.netPendingDelta == 0);
  CHECK(repository.enqueue({.eventId = "dup:2",
                            .payload = "{}",
                            .cameraId = 4,
                            .cooldownClasses = {"k"},
                            .nowMs = 6500,
                            .cooldownMs = 5000,
                            .maxPending = 100})
            .result == ObjectEventEnqueueResult::Recorded);

  drogon::app().quit();
  runner.join();
  std::remove(kOutboxDb);
  std::remove((std::string(kOutboxDb) + "-wal").c_str());
  std::remove((std::string(kOutboxDb) + "-shm").c_str());
}
