#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <drogon/drogon.h>
#include <feature/session/repositories/change-outbox/change-outbox-key.hxx>
#include <feature/session/repositories/change-outbox/change-outbox-repository.hxx>
#include <memory>
#include <sqlite/db-service.hxx>
#include <string>
#include <string_view>
#include <thread>
#include <unistd.h>

#ifndef ARGUS_AUTH_SCHEMA_PATH
#error "ARGUS_AUTH_SCHEMA_PATH must point at the auth schema.sql"
#endif

namespace
{

constexpr const char* kSubject = "argus.identity.change";
constexpr const char* kLegacyOutboxTableDdl = R"(
CREATE TABLE change_outbox (
    id          INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    subject     TEXT    NOT NULL,
    fingerprint TEXT    NOT NULL  DEFAULT '',
    payload     TEXT    NOT NULL,
    status      TEXT    NOT NULL  DEFAULT 'pending'
                        CHECK (status IN ('pending', 'sent')),
    attempts    INTEGER NOT NULL  DEFAULT 0,
    created_at  INTEGER NOT NULL  DEFAULT 0,
    sent_at     INTEGER NOT NULL  DEFAULT 0
))";
constexpr const char* kLegacyOutboxIndexDdl =
    "CREATE INDEX idx_change_outbox_status ON change_outbox (status, id)";

struct ColumnShape
{
  bool present{false};
  bool textNotNullDefaultEmpty{false};
};

[[nodiscard]] int tempCounter()
{
  static std::atomic<int> counter{0};
  return counter.fetch_add(1);
}

class TempDb
{
public:
  explicit TempDb(const char* stem)
      : path_(std::string(stem) + "-" + std::to_string(::getpid()) + "-" +
              std::to_string(tempCounter()) + ".db")
  {
  }

  ~TempDb()
  {
    std::remove(path_.c_str());
    std::remove((path_ + "-wal").c_str());
    std::remove((path_ + "-shm").c_str());
  }

  TempDb(const TempDb&) = delete;
  TempDb& operator=(const TempDb&) = delete;

  [[nodiscard]] const std::string& path() const { return path_; }

private:
  std::string path_;
};

[[nodiscard]] ColumnShape columnShape(std::string_view table,
                                     std::string_view column)
{
  const auto rows = DbService::client()->execSqlSync(
      "SELECT COUNT(*) AS total, COALESCE(SUM(type = 'TEXT' AND "
      "\"notnull\" = 1 AND dflt_value = quote('')), 0) AS strict "
      "FROM pragma_table_info(?) WHERE name = ?",
      std::string(table), std::string(column));
  const auto& row = rows.front();
  return ColumnShape{.present = row["total"].as<int64_t>() > 0,
                     .textNotNullDefaultEmpty = row["strict"].as<int64_t>() > 0};
}

[[nodiscard]] bool hasIndex(std::string_view name)
{
  const auto rows = DbService::client()->execSqlSync(
      "SELECT COUNT(*) AS total FROM sqlite_master "
      "WHERE type = 'index' AND name = ?",
      std::string(name));
  return rows.front()["total"].as<int64_t>() > 0;
}

[[nodiscard]] int64_t insertLegacyRow(std::string_view payload)
{
  const auto result = DbService::client()->execSqlSync(
      "INSERT INTO change_outbox (subject, payload, status, "
      "attempts, created_at, sent_at) "
      "VALUES (?, ?, 'pending', 0, 1, 0)",
      std::string(kSubject), std::string(payload));
  return static_cast<int64_t>(result.insertId());
}

class Fixture
{
public:
  Fixture() = default;

  ~Fixture()
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

  Fixture(const Fixture&) = delete;
  Fixture& operator=(const Fixture&) = delete;

  [[nodiscard]] bool start()
  {
    if (started_)
      return ready_;
    started_ = true;
    drogon::app().setLogLevel(trantor::Logger::kWarn);
    drogon::app().addDbClient(
        drogon::orm::Sqlite3Config{.connectionNumber = 1,
                                   .filename = db_.path(),
                                   .name = "default",
                                   .timeout = -1});
    runner_ = std::thread([] { drogon::app().run(); });
    if (!waitForBoot())
      return false;
    ready_ = DbService::runScriptFile(ARGUS_AUTH_SCHEMA_PATH);
    return ready_;
  }

private:
  [[nodiscard]] static bool waitForBoot()
  {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    while (std::chrono::steady_clock::now() < deadline) {
      if (drogon::app().isRunning())
        return true;
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return drogon::app().isRunning();
  }

  TempDb db_{"auth-change-outbox-test"};
  std::thread runner_;
  bool started_{false};
  bool ready_{false};
};

[[nodiscard]] std::unique_ptr<Fixture>& fixtureStorage()
{
  static std::unique_ptr<Fixture> booted;
  return booted;
}

[[nodiscard]] Fixture& fixture()
{
  auto& booted = fixtureStorage();
  if (!booted)
    booted = std::make_unique<Fixture>();
  return *booted;
}

void stopFixture()
{
  fixtureStorage().reset();
}

[[nodiscard]] const std::array<unsigned char, 16>& entropy()
{
  static const std::array<unsigned char, 16> bytes{0x00, 0x11, 0x22, 0x33,
                                                   0x44, 0x55, 0x66, 0x77,
                                                   0x88, 0x99, 0xAA, 0xBB,
                                                   0xCC, 0xDD, 0xEE, 0xFF};
  return bytes;
}

}

TEST_CASE("the change outbox carries a minted event id and reads it back")
{
  REQUIRE(fixture().start());

  const auto shape = columnShape("change_outbox", "event_id");
  CHECK(shape.present);
  CHECK(shape.textNotNullDefaultEmpty);
  CHECK(hasIndex("idx_change_outbox_event_id"));
  CHECK(hasIndex("idx_change_outbox_status"));

  const ChangeOutboxRepository repository;
  const std::string minted = change_outbox_key::actionMsgId(entropy());
  REQUIRE(minted.size() == change_outbox_key::kActionPrefix.size() + 32);
  CHECK(minted.starts_with(change_outbox_key::kActionPrefix));

  drogon::sync_wait(repository.enqueueAction({.eventId = minted,
                                              .subject = kSubject,
                                              .fingerprint = "sha",
                                              .payload = R"({"userId":1})",
                                              .at = 11,
                                              .client = nullptr}));

  const auto pending = repository.pendingBatch(10);
  REQUIRE(pending.size() == 1);
  CHECK(pending.front().eventId == minted);
  CHECK(pending.front().subject == kSubject);
  CHECK(pending.front().payload == R"({"userId":1})");
  CHECK(pending.front().eventId !=
        change_outbox_key::legacyActionMsgId(pending.front().id));

  bool refusedDuplicate = false;
  try {
    drogon::sync_wait(repository.enqueueAction({.eventId = minted,
                                                .subject = kSubject,
                                                .fingerprint = "",
                                                .payload = "{}",
                                                .at = 12,
                                                .client = nullptr}));
  }
  catch (const std::exception&) {
    refusedDuplicate = true;
  }
  CHECK(refusedDuplicate);

  CHECK(repository.markSent(pending.front().id, 20));
}

TEST_CASE("a legacy change outbox widens under the guard and keeps its rows")
{
  REQUIRE(fixture().start());

  DbService::client()->execSqlSync("DROP TABLE change_outbox");
  DbService::client()->execSqlSync(kLegacyOutboxTableDdl);
  DbService::client()->execSqlSync(kLegacyOutboxIndexDdl);
  const int64_t legacyId = insertLegacyRow(R"({"userId":2})");
  REQUIRE(legacyId > 0);
  REQUIRE_FALSE(columnShape("change_outbox", "event_id").present);

  CHECK_FALSE(DbService::runScriptFile(ARGUS_AUTH_SCHEMA_PATH));

  const ChangeOutboxRepository repository;
  CHECK(repository.migrateLegacySchema());
  const auto widened = columnShape("change_outbox", "event_id");
  CHECK(widened.present);
  CHECK(widened.textNotNullDefaultEmpty);
  CHECK(repository.migrateLegacySchema());

  CHECK(DbService::runScriptFile(ARGUS_AUTH_SCHEMA_PATH));
  CHECK(hasIndex("idx_change_outbox_event_id"));

  const auto legacy = repository.pendingBatch(10);
  REQUIRE(legacy.size() == 1);
  CHECK(legacy.front().id == legacyId);
  CHECK(legacy.front().eventId.empty());
  const std::string fallback =
      change_outbox_key::legacyActionMsgId(legacy.front().id);
  CHECK(fallback == "auth-action:" + std::to_string(legacy.front().id));
  CHECK(fallback != change_outbox_key::actionMsgId(entropy()));

  const std::string minted = change_outbox_key::actionMsgId(entropy());
  drogon::sync_wait(repository.enqueueAction({.eventId = minted,
                                              .subject = kSubject,
                                              .fingerprint = "",
                                              .payload = "{}",
                                              .at = 30,
                                              .client = nullptr}));
  const auto pending = repository.pendingBatch(10);
  REQUIRE(pending.size() == 2);
  CHECK(pending.front().eventId.empty());
  CHECK(pending.back().eventId == minted);
}

int main(int argc, char** argv)
{
  doctest::Context context(argc, argv);
  const int result = context.run();
  stopFixture();
  return result;
}
