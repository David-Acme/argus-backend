#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <shared/repositories/change-outbox/change-outbox-key.hxx>
#include <shared/repositories/change-outbox/change-outbox-repository.hxx>
#include <sqlite/db-service.hxx>

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifndef ARGUS_PRODUCTIVITY_SCHEMA
#error "ARGUS_PRODUCTIVITY_SCHEMA must point at database/schema.sql"
#endif

namespace
{
constexpr const char* kOutboxDb = "productivity-change-outbox-test.db";

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

ChangeOutboxRow pendingRow(const std::vector<ChangeOutboxRow>& rows)
{
  REQUIRE(!rows.empty());
  return rows.empty() ? ChangeOutboxRow{} : rows.front();
}
}

TEST_CASE("the event id names the transition and the fingerprint its payload")
{
  const std::string created = change_outbox_key::eventId(
      {.table = "project", .recordId = 7, .discriminator = R"({"name":"Gate"})"});
  CHECK(created == change_outbox_key::eventId({.table = "project",
                                               .recordId = 7,
                                               .discriminator =
                                                   R"({"name":"Gate"})"}));
  CHECK(created != change_outbox_key::eventId({.table = "project",
                                               .recordId = 8,
                                               .discriminator =
                                                   R"({"name":"Gate"})"}));
  CHECK(created != change_outbox_key::eventId({.table = "project_member",
                                               .recordId = 7,
                                               .discriminator =
                                                   R"({"name":"Gate"})"}));
  CHECK(created.rfind("productivity-change:", 0) == 0);
  CHECK(created.size() == 52);

  CHECK(change_outbox_key::eventId(
            {.table = "project", .recordId = 17, .discriminator = "gate"}) !=
        change_outbox_key::eventId(
            {.table = "project", .recordId = 1, .discriminator = "7gate"}));

  CHECK(change_outbox_key::eventId(
            {.table = "project",
             .recordId = 7,
             .discriminator = R"({"status":"active"})"}) !=
        change_outbox_key::eventId(
            {.table = "project",
             .recordId = 7,
             .discriminator = R"({"status":"paused"})"}));

  Json::Value payload;
  payload["id"] = 7;
  payload["name"] = "Gate";
  CHECK(change_outbox_key::fingerprint(payload) ==
        change_outbox_key::fingerprint(payload));
  Json::Value other = payload;
  other["name"] = "Fence";
  CHECK(change_outbox_key::fingerprint(payload) !=
        change_outbox_key::fingerprint(other));
}

TEST_CASE("the change outbox replays one transition and refuses a conflict")
{
  std::remove(kOutboxDb);
  std::remove((std::string(kOutboxDb) + "-wal").c_str());
  std::remove((std::string(kOutboxDb) + "-shm").c_str());
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{.connectionNumber = 1,
                                 .filename = kOutboxDb,
                                 .name = "default",
                                 .timeout = -1});
  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));
  REQUIRE(DbService::runScriptFile(ARGUS_PRODUCTIVITY_SCHEMA));

  ChangeOutboxRepository repository;

  const ChangeOutboxEnqueueInput incomplete = {.eventId = "",
                                               .fingerprint = "fp",
                                               .payload = "{}",
                                               .at = 0};
  CHECK_THROWS_AS(drogon::sync_wait(repository.enqueue(incomplete)),
                  std::invalid_argument);

  const ChangeOutboxEnqueueInput first = {.eventId = "productivity-change:a",
                                          .fingerprint = "fp-1",
                                          .payload = R"({"info":1})",
                                          .at = 1000};
  const ChangeOutboxEnqueueInput second = {.eventId = "productivity-change:b",
                                           .fingerprint = "fp-2",
                                           .payload = R"({"info":2})",
                                           .at = 2000};
  CHECK(drogon::sync_wait(repository.enqueue(first)) ==
        ChangeOutboxDisposition::Enqueued);
  CHECK(drogon::sync_wait(repository.enqueue(second)) ==
        ChangeOutboxDisposition::Enqueued);

  const ChangeOutboxRow oldest = pendingRow(repository.pendingBatch(1));
  CHECK(oldest.eventId == "productivity-change:a");

  const ChangeOutboxEnqueueInput raced = {.eventId = "productivity-change:a",
                                          .fingerprint = "fp-9",
                                          .payload = R"({"info":9})",
                                          .at = 3000};
  CHECK(drogon::sync_wait(repository.enqueue(first)) ==
        ChangeOutboxDisposition::Replay);
  CHECK(drogon::sync_wait(repository.enqueue(raced)) ==
        ChangeOutboxDisposition::Conflict);

  const ChangeOutboxRow kept = pendingRow(repository.pendingBatch(1));
  CHECK(kept.eventId == "productivity-change:a");
  CHECK(kept.payload == R"({"info":1})");
  CHECK(kept.attempts == 0);

  CHECK(repository.markSent("productivity-change:a", 4000));
  CHECK_FALSE(repository.markSent("productivity-change:a", 4001));

  CHECK(pendingRow(repository.pendingBatch(1)).eventId ==
        "productivity-change:b");
  CHECK(repository.recordAttempt("productivity-change:b"));
  CHECK_FALSE(repository.recordAttempt("productivity-change:a"));
  CHECK(pendingRow(repository.pendingBatch(1)).attempts == 1);
  CHECK(repository.recordAttempt("productivity-change:b"));
  CHECK(pendingRow(repository.pendingBatch(1)).attempts == 2);

  const ChangeOutboxEnqueueInput third = {.eventId = "productivity-change:c",
                                          .fingerprint = "fp-3",
                                          .payload = R"({"info":3})",
                                          .at = 5000};
  const ChangeOutboxEnqueueInput fourth = {.eventId = "productivity-change:d",
                                           .fingerprint = "fp-4",
                                           .payload = R"({"info":4})",
                                           .at = 5000};
  CHECK(drogon::sync_wait(repository.enqueue(third)) ==
        ChangeOutboxDisposition::Enqueued);
  CHECK(drogon::sync_wait(repository.enqueue(fourth)) ==
        ChangeOutboxDisposition::Enqueued);
  CHECK(repository.markSent("productivity-change:b", 5100));
  CHECK(pendingRow(repository.pendingBatch(1)).eventId ==
        "productivity-change:c");
  CHECK(repository.markSent("productivity-change:c", 5200));
  CHECK(pendingRow(repository.pendingBatch(1)).eventId ==
        "productivity-change:d");

  std::remove(kOutboxDb);
  std::remove((std::string(kOutboxDb) + "-wal").c_str());
  std::remove((std::string(kOutboxDb) + "-shm").c_str());
}
