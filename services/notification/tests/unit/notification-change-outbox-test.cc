#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <shared/repositories/change-outbox/change-outbox-key.hxx>
#include <shared/repositories/change-outbox/change-outbox-repository.hxx>
#include <sqlite/db-service.hxx>

#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

#ifndef ARGUS_NOTIFICATION_SCHEMA
#error "ARGUS_NOTIFICATION_SCHEMA must point at database/schema.sql"
#endif

namespace
{
constexpr const char* kOutboxDb = "notification-change-outbox-test.db";

class AppRunner
{
public:
  AppRunner() : runner_([] { drogon::app().run(); }) {}

  ~AppRunner()
  {
    if (!runner_.joinable())
      return;
    // Drogon reports the app running before its main loop is looping, and a
    // loop that has not begun cannot be stopped: trantor's loop() clears the
    // quit flag again as it starts. Waiting for it to loop is what makes the
    // quit below take effect — detaching in that window left the app's thread
    // running past the end of the process, measured as SIGSEGV inside
    // EventLoop::loop() in 3 of 20 runs of a forced constructor throw.
    for (int i = 0; i < 3000 && !drogon::app().getLoop()->isRunning(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (drogon::app().getLoop()->isRunning()) {
      drogon::app().quit();
      runner_.join();
      return;
    }
    // A boot that never reached the loop at all is left to the process: it
    // cannot be asked to stop, and joining it would block for ever.
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

// The head pending row, reported as a failed assertion when the outbox holds none.
ChangeOutboxRow pendingRow(const std::vector<ChangeOutboxRow>& rows)
{
  REQUIRE(!rows.empty());
  return rows.empty() ? ChangeOutboxRow{} : rows.front();
}
} // namespace

TEST_CASE("the event id names the transition and the fingerprint its payload")
{
  const std::string read = change_outbox_key::eventId(
      {.table = "notification", .recordId = 7, .discriminator = R"({"isRead":1})"});
  CHECK(read == change_outbox_key::eventId({.table = "notification",
                                            .recordId = 7,
                                            .discriminator =
                                                R"({"isRead":1})"}));
  CHECK(read != change_outbox_key::eventId({.table = "notification",
                                            .recordId = 8,
                                            .discriminator = R"({"isRead":1})"}));
  CHECK(read != change_outbox_key::eventId({.table = "notification_token",
                                            .recordId = 7,
                                            .discriminator = R"({"isRead":1})"}));
  CHECK(read.rfind("notification-change:", 0) == 0);
  // The MsgId travels as a JetStream header and stays inside its budget.
  CHECK(read.size() == 52);

  // The discriminator is joined by a separator, so a record id cannot run into
  // it and name another record's event.
  CHECK(change_outbox_key::eventId(
            {.table = "notification", .recordId = 17, .discriminator = "read"}) !=
        change_outbox_key::eventId(
            {.table = "notification", .recordId = 1, .discriminator = "7read"}));

  // A transition is discriminated by its own payload, so a record that moves
  // again — or returns to a state it already held — is a new event.
  CHECK(change_outbox_key::eventId(
            {.table = "notification",
             .recordId = 7,
             .discriminator = R"({"isRead":0})"}) !=
        change_outbox_key::eventId(
            {.table = "notification",
             .recordId = 7,
             .discriminator = R"({"isRead":1})"}));

  Json::Value payload;
  payload["id"] = 7;
  payload["isRead"] = 1;
  CHECK(change_outbox_key::fingerprint(payload) ==
        change_outbox_key::fingerprint(payload));
  Json::Value other = payload;
  other["isRead"] = 0;
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
  REQUIRE(DbService::runScriptFile(ARGUS_NOTIFICATION_SCHEMA));

  ChangeOutboxRepository repository;

  const ChangeOutboxEnqueueInput incomplete = {.eventId = "",
                                               .fingerprint = "fp",
                                               .payload = "{}",
                                               .at = 0};
  CHECK(drogon::sync_wait(repository.enqueue(incomplete)) ==
        ChangeOutboxDisposition::Failed);

  const ChangeOutboxEnqueueInput first = {.eventId = "notification-change:a",
                                          .fingerprint = "fp-1",
                                          .payload = R"({"info":1})",
                                          .at = 1000};
  const ChangeOutboxEnqueueInput second = {.eventId = "notification-change:b",
                                           .fingerprint = "fp-2",
                                           .payload = R"({"info":2})",
                                           .at = 2000};
  CHECK(drogon::sync_wait(repository.enqueue(first)) ==
        ChangeOutboxDisposition::Enqueued);
  CHECK(drogon::sync_wait(repository.enqueue(second)) ==
        ChangeOutboxDisposition::Enqueued);

  const ChangeOutboxRow oldest = pendingRow(repository.pendingBatch(1));
  CHECK(oldest.eventId == "notification-change:a");

  const ChangeOutboxEnqueueInput raced = {.eventId = "notification-change:a",
                                          .fingerprint = "fp-9",
                                          .payload = R"({"info":9})",
                                          .at = 3000};
  CHECK(drogon::sync_wait(repository.enqueue(first)) ==
        ChangeOutboxDisposition::Replay);
  CHECK(drogon::sync_wait(repository.enqueue(raced)) ==
        ChangeOutboxDisposition::Conflict);

  const ChangeOutboxRow kept = pendingRow(repository.pendingBatch(1));
  CHECK(kept.eventId == "notification-change:a");
  CHECK(kept.payload == R"({"info":1})");
  CHECK(kept.attempts == 0);

  CHECK(repository.markSent("notification-change:a", 4000));
  CHECK_FALSE(repository.markSent("notification-change:a", 4001));

  CHECK(pendingRow(repository.pendingBatch(1)).eventId == "notification-change:b");
  CHECK(repository.recordAttempt("notification-change:b"));
  CHECK_FALSE(repository.recordAttempt("notification-change:a"));
  CHECK(pendingRow(repository.pendingBatch(1)).attempts == 1);
  CHECK(repository.recordAttempt("notification-change:b"));
  CHECK(pendingRow(repository.pendingBatch(1)).attempts == 2);

  // Two changes made in the same millisecond drain one at a time in the order
  // they were enqueued. That both rows are pending at once is what this pins;
  // the id tiebreak under the query's ORDER BY is not observable from here,
  // because the (status, created_at) index hands equal keys back in rowid order
  // whether the clause is written or not.
  const ChangeOutboxEnqueueInput third = {.eventId = "notification-change:c",
                                          .fingerprint = "fp-3",
                                          .payload = R"({"info":3})",
                                          .at = 5000};
  const ChangeOutboxEnqueueInput fourth = {.eventId = "notification-change:d",
                                           .fingerprint = "fp-4",
                                           .payload = R"({"info":4})",
                                           .at = 5000};
  CHECK(drogon::sync_wait(repository.enqueue(third)) ==
        ChangeOutboxDisposition::Enqueued);
  CHECK(drogon::sync_wait(repository.enqueue(fourth)) ==
        ChangeOutboxDisposition::Enqueued);
  CHECK(repository.markSent("notification-change:b", 5100));
  CHECK(pendingRow(repository.pendingBatch(1)).eventId == "notification-change:c");
  CHECK(repository.markSent("notification-change:c", 5200));
  CHECK(pendingRow(repository.pendingBatch(1)).eventId == "notification-change:d");

  std::remove(kOutboxDb);
  std::remove((std::string(kOutboxDb) + "-wal").c_str());
  std::remove((std::string(kOutboxDb) + "-shm").c_str());
}
