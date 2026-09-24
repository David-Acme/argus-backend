#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <shared/repositories/change-outbox/change-outbox-key.hxx>
#include <shared/repositories/change-outbox/change-outbox-repository.hxx>
#include <sqlite/db-service.hxx>

#include <array>
#include <chrono>
#include <cstdio>
#include <stdexcept>
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

std::string mintedActionId(unsigned char seed)
{
  std::array<unsigned char, 16> bytes{};
  bytes.fill(seed);
  return change_outbox_key::actionMsgId(bytes);
}
}

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
  CHECK(created.size() == 48);

  CHECK(change_outbox_key::eventId(
            {.table = "user", .recordId = 17, .discriminator = "ana"}) !=
        change_outbox_key::eventId(
            {.table = "user", .recordId = 1, .discriminator = "7ana"}));

  CHECK(change_outbox_key::eventId({.table = "user",
                                    .recordId = 7,
                                    .discriminator =
                                        R"({"role":"guest"})"}) !=
        change_outbox_key::eventId(
            {.table = "user",
             .recordId = 7,
             .discriminator = R"({"role":"resident"})"}));

  const std::array<unsigned char, 16> zeroEntropy{};
  const std::string minted = change_outbox_key::actionMsgId(zeroEntropy);
  CHECK(minted == "identity-action:00000000000000000000000000000000");
  CHECK(minted.rfind(change_outbox_key::kActionPrefix, 0) == 0);
  CHECK(minted.size() == change_outbox_key::kActionPrefix.size() + 32);
  std::array<unsigned char, 16> fullEntropy{};
  fullEntropy.fill(0xFF);
  CHECK(change_outbox_key::actionMsgId(fullEntropy) ==
        "identity-action:ffffffffffffffffffffffffffffffff");
  CHECK(change_outbox_key::legacyActionMsgId(7) == "identity-action:7");
  CHECK(change_outbox_key::legacyActionMsgId(7) !=
        change_outbox_key::legacyActionMsgId(8));
  CHECK(change_outbox_key::legacyActionMsgId(7).rfind("identity-action:", 0) ==
        0);
  CHECK(minted != change_outbox_key::legacyActionMsgId(7));

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
                                               .at = 0,
                                               .client = nullptr};
  CHECK_THROWS_AS(drogon::sync_wait(repository.enqueue(incomplete)),
                  std::invalid_argument);

  const ChangeOutboxEnqueueInput first = {.eventId = "identity-change:a",
                                          .subject = kChangeSubject,
                                          .fingerprint = "fp-1",
                                          .payload = R"({"info":1})",
                                          .at = 1000,
                                          .client = nullptr};
  const ChangeOutboxEnqueueInput second = {.eventId = "identity-change:b",
                                           .subject = kChangeSubject,
                                           .fingerprint = "fp-2",
                                           .payload = R"({"info":2})",
                                           .at = 2000,
                                           .client = nullptr};
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
                                          .at = 3000,
                                          .client = nullptr};
  CHECK(drogon::sync_wait(repository.enqueue(first)) ==
        ChangeOutboxDisposition::Replay);
  CHECK(drogon::sync_wait(repository.enqueue(raced)) ==
        ChangeOutboxDisposition::Conflict);

  const ChangeOutboxRow kept = pendingRow(repository.pendingBatch(1));
  CHECK(kept.eventId == "identity-change:a");
  CHECK(kept.payload == R"({"info":1})");
  CHECK(kept.attempts == 0);

  CHECK(repository.markSent(kept.id, 4000));
  CHECK_FALSE(repository.markSent(kept.id, 4001));

  const ChangeOutboxRow last = pendingRow(repository.pendingBatch(1));
  CHECK(last.eventId == "identity-change:b");
  CHECK(repository.recordAttempt(last.id));
  CHECK_FALSE(repository.recordAttempt(kept.id));
  CHECK(pendingRow(repository.pendingBatch(1)).attempts == 1);
  CHECK(repository.recordAttempt(last.id));
  CHECK(pendingRow(repository.pendingBatch(1)).attempts == 2);

  const ChangeOutboxEnqueueInput third = {.eventId = "identity-change:c",
                                          .subject = kChangeSubject,
                                          .fingerprint = "fp-3",
                                          .payload = R"({"info":3})",
                                          .at = 5000,
                                          .client = nullptr};
  const ChangeOutboxEnqueueInput fourth = {.eventId = "identity-change:d",
                                           .subject = kChangeSubject,
                                           .fingerprint = "fp-4",
                                           .payload = R"({"info":4})",
                                           .at = 5000,
                                           .client = nullptr};
  CHECK(drogon::sync_wait(repository.enqueue(third)) ==
        ChangeOutboxDisposition::Enqueued);
  CHECK(drogon::sync_wait(repository.enqueue(fourth)) ==
        ChangeOutboxDisposition::Enqueued);
  CHECK(repository.markSent(last.id, 5100));
  CHECK(pendingRow(repository.pendingBatch(1)).eventId == "identity-change:c");
  CHECK(repository.markSent(
      pendingRow(repository.pendingBatch(1)).id, 5200));
  CHECK(pendingRow(repository.pendingBatch(1)).eventId == "identity-change:d");

  const ChangeOutboxActionInput read = {.eventId = mintedActionId(7),
                                        .subject = kActionSubject,
                                        .fingerprint = "fp-read",
                                        .payload = R"({"action":"read"})",
                                        .at = 6000,
                                        .client = nullptr};
  const ChangeOutboxActionInput readAgain = {.eventId = mintedActionId(8),
                                             .subject = kActionSubject,
                                             .fingerprint = "fp-read",
                                             .payload = R"({"action":"read"})",
                                             .at = 6000,
                                             .client = nullptr};
  drogon::sync_wait(repository.enqueueAction(read));
  drogon::sync_wait(repository.enqueueAction(readAgain));
  CHECK_THROWS_AS(drogon::sync_wait(repository.enqueueAction(
                      {.eventId = mintedActionId(9),
                       .subject = kActionSubject,
                       .fingerprint = "fp",
                       .payload = "",
                       .at = 0,
                       .client = nullptr})),
                  std::invalid_argument);
  CHECK_THROWS_AS(drogon::sync_wait(repository.enqueueAction(
                      {.eventId = "",
                       .subject = kActionSubject,
                       .fingerprint = "fp",
                       .payload = R"({"action":"read"})",
                       .at = 0,
                       .client = nullptr})),
                  std::invalid_argument);

  CHECK(repository.markSent(pendingRow(repository.pendingBatch(1)).id, 6100));
  const ChangeOutboxRow journal = pendingRow(repository.pendingBatch(1));
  CHECK(journal.subject == kActionSubject);
  CHECK(journal.eventId == mintedActionId(7));
  CHECK(journal.payload == R"({"action":"read"})");
  CHECK(journal.id > 0);
  CHECK(journal.eventId != mintedActionId(8));

  CHECK(repository.purgeSent(6100) == 4);
  CHECK(repository.purgeSent(999999) == 0);
  CHECK(pendingRow(repository.pendingBatch(1)).id == journal.id);

  std::remove(kOutboxDb);
  std::remove((std::string(kOutboxDb) + "-wal").c_str());
  std::remove((std::string(kOutboxDb) + "-shm").c_str());
}
