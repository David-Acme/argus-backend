#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <outbox/outbox-repository.hxx>
#include <outbox/outbox-status.hxx>
#include <sqlite/db-service.hxx>

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
constexpr const char* kOutboxDb = "lib-outbox-repository-test.db";

constexpr const char* kKeyedShape =
    "CREATE TABLE change_outbox ("
    "event_id TEXT NOT NULL PRIMARY KEY, "
    "fingerprint TEXT NOT NULL DEFAULT '', "
    "payload TEXT NOT NULL, "
    "status TEXT NOT NULL DEFAULT 'pending' "
    "CHECK (status IN ('pending', 'sent')), "
    "attempts INTEGER NOT NULL DEFAULT 0, "
    "created_at INTEGER NOT NULL DEFAULT 0, "
    "sent_at INTEGER NOT NULL DEFAULT 0)";

constexpr const char* kSequencedShape =
    "CREATE TABLE change_outbox ("
    "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
    "event_id TEXT UNIQUE, "
    "subject TEXT NOT NULL, "
    "fingerprint TEXT NOT NULL DEFAULT '', "
    "payload TEXT NOT NULL, "
    "status TEXT NOT NULL DEFAULT 'pending' "
    "CHECK (status IN ('pending', 'sent')), "
    "attempts INTEGER NOT NULL DEFAULT 0, "
    "created_at INTEGER NOT NULL DEFAULT 0, "
    "sent_at INTEGER NOT NULL DEFAULT 0)";

constexpr const char* kPreEventIdShape =
    "CREATE TABLE change_outbox ("
    "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
    "subject TEXT NOT NULL, "
    "fingerprint TEXT NOT NULL DEFAULT '', "
    "payload TEXT NOT NULL, "
    "status TEXT NOT NULL DEFAULT 'pending' "
    "CHECK (status IN ('pending', 'sent')), "
    "attempts INTEGER NOT NULL DEFAULT 0, "
    "created_at INTEGER NOT NULL DEFAULT 0, "
    "sent_at INTEGER NOT NULL DEFAULT 0)";

void removeDb()
{
  std::remove(kOutboxDb);
  std::remove((std::string(kOutboxDb) + "-wal").c_str());
  std::remove((std::string(kOutboxDb) + "-shm").c_str());
}

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
      removeDb();
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
  while (!drogon::app().isRunning() &&
         std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  return drogon::app().isRunning();
}

void reshape(const char* shape)
{
  DbService::client()->execSqlSync("DROP TABLE IF EXISTS change_outbox");
  DbService::client()->execSqlSync(shape);
}

bool hasColumn(const std::string& column)
{
  const auto rows = DbService::client()->execSqlSync(
      "SELECT COUNT(*) AS total FROM pragma_table_info('change_outbox') "
      "WHERE name = ?",
      column);
  return !rows.empty() && rows.front()["total"].as<int64_t>() > 0;
}

outbox::OutboxRepository repository()
{
  return outbox::OutboxRepository([] { return DbService::client(); });
}

outbox::OutboxInsert insertOf(const std::string& id, const std::string& payload)
{
  return {.eventId = id,
          .subject = "argus.test.change",
          .fingerprint = "fp-" + payload,
          .payload = payload,
          .at = 1000,
          .client = nullptr};
}

outbox::OutboxRow front(const std::vector<outbox::OutboxRow>& rows)
{
  REQUIRE(!rows.empty());
  return rows.empty() ? outbox::OutboxRow{} : rows.front();
}
}

TEST_CASE("the migration brings a keyed table to the shared shape and keeps "
          "its rows")
{
  reshape(kKeyedShape);
  DbService::client()->execSqlSync(
      "INSERT INTO change_outbox (event_id, fingerprint, payload, created_at) "
      "VALUES ('camera-change:legacy', 'fp', '{\"legacy\":1}', 10)");
  REQUIRE_FALSE(hasColumn("subject"));

  const auto outbox = repository();
  REQUIRE(outbox.migrateSchema());
  CHECK(hasColumn("subject"));
  REQUIRE(outbox.migrateSchema());

  const auto legacy = front(outbox.pendingBatch(10));
  CHECK(legacy.eventId == "camera-change:legacy");
  CHECK(legacy.subject.empty());
  CHECK(legacy.payload == R"({"legacy":1})");
  CHECK(legacy.id > 0);
}

TEST_CASE("the migration adds event_id to the journal that predates it")
{
  reshape(kPreEventIdShape);
  DbService::client()->execSqlSync(
      "INSERT INTO change_outbox (subject, payload) "
      "VALUES ('argus.auth.v1.user-action', '{\"old\":1}')");
  REQUIRE_FALSE(hasColumn("event_id"));

  const auto outbox = repository();
  REQUIRE(outbox.migrateSchema());
  CHECK(hasColumn("event_id"));

  const auto legacy = front(outbox.pendingBatch(10));
  CHECK(legacy.eventId.empty());
  CHECK(legacy.subject == "argus.auth.v1.user-action");
  CHECK(legacy.id == 1);
}

TEST_CASE("a missing table is not the migration's to create")
{
  DbService::client()->execSqlSync("DROP TABLE IF EXISTS change_outbox");
  CHECK(repository().migrateSchema());
  CHECK_FALSE(hasColumn("subject"));
}

TEST_CASE("a null event id reads as empty so the relay falls back to the "
          "legacy id")
{
  reshape(kSequencedShape);
  DbService::client()->execSqlSync(
      "INSERT INTO change_outbox (event_id, subject, payload) "
      "VALUES (NULL, 'argus.identity.v1.user-action', '{\"old\":1}')");
  const auto outbox = repository();
  REQUIRE(outbox.migrateSchema());
  const auto legacy = front(outbox.pendingBatch(10));
  CHECK(legacy.eventId.empty());
  CHECK(legacy.subject == "argus.identity.v1.user-action");
}

TEST_CASE("the outbox replays one transition, refuses a conflict and keeps "
          "insertion order on both shapes")
{
  for (const char* shape : {kKeyedShape, kSequencedShape}) {
    CAPTURE(shape);
    reshape(shape);
    const auto outbox = repository();
    REQUIRE(outbox.migrateSchema());

    for (const auto& incomplete : {insertOf("", "{}"), insertOf("a", "")}) {
      CHECK_THROWS_AS(drogon::sync_wait(outbox.insert(incomplete)),
                      std::invalid_argument);
    }
    auto subjectless = insertOf("z", "{}");
    subjectless.subject.clear();
    CHECK_THROWS_AS(drogon::sync_wait(outbox.insert(subjectless)),
                    std::invalid_argument);

    auto later = insertOf("b", R"({"info":2})");
    later.at = 1;
    CHECK(drogon::sync_wait(outbox.insert(insertOf("a", R"({"info":1})"))) ==
          outbox::OutboxDisposition::Enqueued);
    CHECK(drogon::sync_wait(outbox.insert(later)) ==
          outbox::OutboxDisposition::Enqueued);

    const auto oldest = front(outbox.pendingBatch(1));
    CHECK(oldest.eventId == "a");
    CHECK(oldest.subject == "argus.test.change");

    CHECK(drogon::sync_wait(outbox.insert(insertOf("a", R"({"info":1})"))) ==
          outbox::OutboxDisposition::Replay);
    auto raced = insertOf("a", R"({"info":9})");
    CHECK(drogon::sync_wait(outbox.insert(raced)) ==
          outbox::OutboxDisposition::Conflict);
    const auto kept = front(outbox.pendingBatch(1));
    CHECK(kept.payload == R"({"info":1})");
    CHECK(kept.attempts == 0);

    CHECK(outbox.markSent(kept.id, 4000));
    CHECK_FALSE(outbox.markSent(kept.id, 4001));

    const auto next = front(outbox.pendingBatch(1));
    CHECK(next.eventId == "b");
    CHECK(outbox.recordAttempt(next.id));
    CHECK_FALSE(outbox.recordAttempt(kept.id));
    CHECK(front(outbox.pendingBatch(1)).attempts == 1);

    CHECK(drogon::sync_wait(outbox.insert(insertOf("c", R"({"info":3})"))) ==
          outbox::OutboxDisposition::Enqueued);
    CHECK(outbox.markSent(next.id, 5100));
    CHECK(front(outbox.pendingBatch(1)).eventId == "c");
    CHECK(outbox.pendingBatch(0).empty());

    CHECK(outbox.purgeSent(4500) == 1);
    CHECK(outbox.purgeSent(4500) == 0);
    CHECK(outbox.purgeSent(999999) == 1);
    CHECK(front(outbox.pendingBatch(5)).eventId == "c");
    CHECK(outbox.pendingBatch(5).size() == 1);
  }
}

TEST_CASE("an outbox without a client says so instead of crashing")
{
  const outbox::OutboxRepository orphan([] {
    return drogon::orm::DbClientPtr{};
  });
  CHECK_THROWS_AS(static_cast<void>(orphan.pendingBatch(1)),
                  std::runtime_error);
  CHECK_FALSE(orphan.migrateSchema());
}

int main(int argc, char** argv)
{
  removeDb();
  drogon::app().setLogLevel(trantor::Logger::kFatal);
  drogon::app().addDbClient(
      drogon::orm::Sqlite3Config{.connectionNumber = 1,
                                 .filename = kOutboxDb,
                                 .name = "default",
                                 .timeout = -1});
  const AppRunner runner;
  if (!waitForBoot(std::chrono::seconds(30)))
    return 1;
  doctest::Context context(argc, argv);
  return context.run();
}
