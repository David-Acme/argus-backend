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

#ifndef ARGUS_IDENTITY_SCHEMA
#error "ARGUS_IDENTITY_SCHEMA must point at database/schema.sql"
#endif

namespace
{
constexpr const char* kOutboxDb = "identity-change-outbox-test.db";
constexpr const char* kChangeSubject = "argus.identity.v1.change";
constexpr const char* kActionSubject = "argus.identity.v1.user-action";

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
    // quit below take effect -- detaching in that window left the app's thread
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

// The head pending row, reported as a failed assertion when the outbox holds
// none.
ChangeOutboxRow pendingRow(const std::vector<ChangeOutboxRow>& rows)
{
  REQUIRE(!rows.empty());
  return rows.empty() ? ChangeOutboxRow{} : rows.front();
}
} // namespace

TEST_CASE("the event id names the transition and the action id its own row")
{
  const std::string created = change_outbox_key::eventId(
      {.table = "user", .recordId = 7, .discriminator = R"({"name":"Ana"})"});
  CHECK(created == change_outbox_key::eventId(
                       {.table = "user",
                        .recordId = 7,
                        .discriminator = R"({"name":"Ana"})"}));
  CHECK(created != change_outbox_key::eventId(
                       {.table = "user",
                        .recordId = 8,
                        .discriminator = R"({"name":"Ana"})"}));
  CHECK(created != change_outbox_key::eventId(
                       {.table = "person",
                        .recordId = 7,
                        .discriminator = R"({"name":"Ana"})"}));
  CHECK(created.rfind("identity-change:", 0) == 0);
  // The MsgId travels as a JetStream header and stays inside its budget.
  CHECK(created.size() == 48);

  // The discriminator is joined by a separator, so a record id cannot run into
  // it and name another record's event.
  CHECK(change_outbox_key::eventId(
            {.table = "user", .recordId = 17, .discriminator = "ana"}) !=
        change_outbox_key::eventId(
            {.table = "user", .recordId = 1, .discriminator = "7ana"}));

  // A transition is discriminated by its own payload, so a record that moves
  // again -- or returns to a state it already held -- is a new event.
  CHECK(change_outbox_key::eventId({.table = "user",
                                    .recordId = 7,
                                    .discriminator =
                                        R"({"role":"guest"})"}) !=
        change_outbox_key::eventId(
            {.table = "user",
             .recordId = 7,
             .discriminator = R"({"role":"resident"})"}));

  // The journal leg keys on the outbox row's own position: two reads of one
  // record are two audit rows, and this is what tells them apart.
  CHECK(change_outbox_key::actionMsgId(7) == "identity-action:7");
  CHECK(change_outbox_key::actionMsgId(7) != change_outbox_key::actionMsgId(8));
  CHECK(change_outbox_key::actionMsgId(7).rfind("identity-action:", 0) == 0);

  Json::Value payload;
  payload["id"] = 7;
  payload["name"] = "Ana";
  CHECK(change_outbox_key::fingerprint(payload) ==
        change_outbox_key::fingerprint(payload));
  Json::Value other = payload;
  other["name"] = "Luis";
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
  REQUIRE(DbService::runScriptFile(ARGUS_IDENTITY_SCHEMA));

  ChangeOutboxRepository repository;

  const ChangeOutboxEnqueueInput incomplete = {.eventId = "",
                                               .subject = kChangeSubject,
                                               .fingerprint = "fp",
                                               .payload = "{}",
                                               .at = 0};
  CHECK(drogon::sync_wait(repository.enqueue(incomplete)) ==
        ChangeOutboxDisposition::Failed);

  const ChangeOutboxEnqueueInput first = {.eventId = "identity-change:a",
                                          .subject = kChangeSubject,
                                          .fingerprint = "fp-1",
                                          .payload = R"({"info":1})",
                                          .at = 1000};
  const ChangeOutboxEnqueueInput second = {.eventId = "identity-change:b",
                                           .subject = kChangeSubject,
                                           .fingerprint = "fp-2",
                                           .payload = R"({"info":2})",
                                           .at = 2000};
  CHECK(drogon::sync_wait(repository.enqueue(first)) ==
        ChangeOutboxDisposition::Enqueued);
  CHECK(drogon::sync_wait(repository.enqueue(second)) ==
        ChangeOutboxDisposition::Enqueued);

  const ChangeOutboxRow oldest = pendingRow(repository.pendingBatch(1));
  CHECK(oldest.eventId == "identity-change:a");
  CHECK(oldest.subject == kChangeSubject);
  CHECK(oldest.id == 1);

  const ChangeOutboxEnqueueInput raced = {.eventId = "identity-change:a",
                                          .subject = kChangeSubject,
                                          .fingerprint = "fp-9",
                                          .payload = R"({"info":9})",
                                          .at = 3000};
  CHECK(drogon::sync_wait(repository.enqueue(first)) ==
        ChangeOutboxDisposition::Replay);
  CHECK(drogon::sync_wait(repository.enqueue(raced)) ==
        ChangeOutboxDisposition::Conflict);

  const ChangeOutboxRow kept = pendingRow(repository.pendingBatch(1));
  CHECK(kept.eventId == "identity-change:a");
  CHECK(kept.payload == R"({"info":1})");
  CHECK(kept.attempts == 0);

  // Settlement is a status-guarded CAS over the row's own id, because a
  // journal row has no event id to guard on.
  CHECK(repository.markSent(kept.id, 4000));
  CHECK_FALSE(repository.markSent(kept.id, 4001));

  const ChangeOutboxRow last = pendingRow(repository.pendingBatch(1));
  CHECK(last.eventId == "identity-change:b");
  CHECK(repository.recordAttempt(last.id));
  CHECK_FALSE(repository.recordAttempt(kept.id));
  CHECK(pendingRow(repository.pendingBatch(1)).attempts == 1);
  CHECK(repository.recordAttempt(last.id));
  CHECK(pendingRow(repository.pendingBatch(1)).attempts == 2);

  // Two changes made in the same millisecond drain one at a time in the order
  // they were enqueued. That both rows are pending at once is what this pins;
  // the id tiebreak under the query's ORDER BY is not observable from here,
  // because the (status, id) index hands equal keys back in id order whether
  // the clause is written or not.
  const ChangeOutboxEnqueueInput third = {.eventId = "identity-change:c",
                                          .subject = kChangeSubject,
                                          .fingerprint = "fp-3",
                                          .payload = R"({"info":3})",
                                          .at = 5000};
  const ChangeOutboxEnqueueInput fourth = {.eventId = "identity-change:d",
                                           .subject = kChangeSubject,
                                           .fingerprint = "fp-4",
                                           .payload = R"({"info":4})",
                                           .at = 5000};
  CHECK(drogon::sync_wait(repository.enqueue(third)) ==
        ChangeOutboxDisposition::Enqueued);
  CHECK(drogon::sync_wait(repository.enqueue(fourth)) ==
        ChangeOutboxDisposition::Enqueued);
  CHECK(repository.markSent(last.id, 5100));
  CHECK(pendingRow(repository.pendingBatch(1)).eventId == "identity-change:c");
  CHECK(repository.markSent(
      pendingRow(repository.pendingBatch(1)).id, 5200));
  CHECK(pendingRow(repository.pendingBatch(1)).eventId == "identity-change:d");

  // The journal leg: identical payloads are two rows, not a replay and never a
  // conflict, and each carries the position the drain derives its MsgId from.
  const ChangeOutboxActionInput read = {.subject = kActionSubject,
                                        .fingerprint = "fp-read",
                                        .payload = R"({"action":"read"})",
                                        .at = 6000};
  CHECK(drogon::sync_wait(repository.enqueueAction(read)));
  CHECK(drogon::sync_wait(repository.enqueueAction(read)));
  CHECK_FALSE(drogon::sync_wait(repository.enqueueAction(
      {.subject = kActionSubject, .fingerprint = "fp", .payload = "", .at = 0})));

  CHECK(repository.markSent(pendingRow(repository.pendingBatch(1)).id, 6100));
  const ChangeOutboxRow journal = pendingRow(repository.pendingBatch(1));
  CHECK(journal.subject == kActionSubject);
  CHECK(journal.eventId.empty());
  CHECK(journal.payload == R"({"action":"read"})");
  CHECK(journal.id > 0);
  // The two rows are one action each: the derived id is what keeps the second
  // from being deduplicated against the first.
  CHECK(change_outbox_key::actionMsgId(journal.id) !=
        change_outbox_key::actionMsgId(journal.id - 1));

  std::remove(kOutboxDb);
  std::remove((std::string(kOutboxDb) + "-wal").c_str());
  std::remove((std::string(kOutboxDb) + "-shm").c_str());
}
