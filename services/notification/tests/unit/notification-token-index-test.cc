#include <doctest/doctest.h>

#include <shared/repositories/notification-token/notification-token-index.hxx>
#include "../support/sqlite-token-index-sql.hxx"

#include <sqlite3.h>

#include <cstdint>
#include <string>
#include <string_view>

namespace
{
constexpr const char* kOldestTableWithRow = R"sql(
CREATE TABLE notification_token (
    id          INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    user_id     INTEGER NOT NULL,
    device_hash TEXT    NOT NULL  DEFAULT '',
    token       TEXT    NOT NULL  UNIQUE,
    platform    TEXT    NOT NULL  DEFAULT '',
    lang        TEXT    NOT NULL  DEFAULT '',
    is_active   INTEGER NOT NULL  DEFAULT 1,
    created_at  INTEGER NOT NULL  DEFAULT 0,
    updated_at  INTEGER
);
CREATE UNIQUE INDEX idx_notification_token_uniq ON notification_token (user_id, device_hash);
INSERT INTO notification_token (user_id, device_hash, token, platform, lang)
    VALUES (1, 'device-a', 'token-apns-1', 'ios', 'es');
)sql";

constexpr const char* kTableWithRow = R"sql(
CREATE TABLE notification_token (
    id          INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    user_id     INTEGER NOT NULL,
    device_hash TEXT    NOT NULL  DEFAULT '',
    token       TEXT    NOT NULL  UNIQUE,
    platform    TEXT    NOT NULL  DEFAULT '',
    lang        TEXT    NOT NULL  DEFAULT '',
    is_active   INTEGER NOT NULL  DEFAULT 1,
    created_at  INTEGER NOT NULL  DEFAULT 0,
    updated_at  INTEGER,
    session_id  TEXT    NOT NULL  DEFAULT '',
    provider    TEXT    NOT NULL  DEFAULT '',
    public_key  TEXT    NOT NULL  DEFAULT '',
    key_id      TEXT    NOT NULL  DEFAULT ''
);
CREATE UNIQUE INDEX idx_notification_token_uniq ON notification_token (user_id, device_hash);
INSERT INTO notification_token (user_id, device_hash, token, platform, lang, provider, public_key, key_id)
    VALUES (1, 'device-a', 'token-apns-1', 'ios', 'es', 'apns', 'pk', 'kid-apns');
)sql";

struct OpenDatabase
{
  sqlite3* db{nullptr};

  explicit OpenDatabase(const char* schema)
  {
    sqlite3_open(":memory:", &db);
    sqlite3_exec(db, schema, nullptr, nullptr, nullptr);
  }

  ~OpenDatabase() { sqlite3_close(db); }

  OpenDatabase(const OpenDatabase&) = delete;
  OpenDatabase& operator=(const OpenDatabase&) = delete;
};

int64_t rootPageOf(sqlite3* db)
{
  sqlite3_stmt* statement = nullptr;
  sqlite3_prepare_v2(db,
                     "SELECT rootpage FROM sqlite_master WHERE type = 'table' AND name = 'notification_token'",
                     -1, &statement, nullptr);
  int64_t page = -1;
  if (sqlite3_step(statement) == SQLITE_ROW)
    page = sqlite3_column_int64(statement, 0);
  sqlite3_finalize(statement);
  return page;
}
}

TEST_CASE("the migration drops the legacy index, adds the device index, keeps the row and lets one device hold an apns and an apns_voip row")
{
  OpenDatabase database(kTableWithRow);
  SqliteTokenIndexSql sql(database.db);
  const int64_t tableBefore = rootPageOf(database.db);
  REQUIRE(sql.countRows("SELECT COUNT(*) FROM sqlite_master WHERE type = 'index' AND name = 'idx_notification_token_uniq'") == 1);
  REQUIRE(sql.countRows("SELECT COUNT(*) FROM sqlite_master WHERE type = 'index' AND name = 'idx_notification_token_device'") == 0);

  const auto result = notification_token_index::migrate(sql);
  CHECK(result.droppedLegacyIndex);
  CHECK(result.columnsAdded == 0);
  CHECK(result.changed());

  CHECK(sql.countRows("SELECT COUNT(*) FROM sqlite_master WHERE type = 'index' AND name = 'idx_notification_token_uniq'") == 0);
  CHECK(sql.countRows("SELECT COUNT(*) FROM sqlite_master WHERE type = 'index' AND name = 'idx_notification_token_device'") == 1);
  CHECK(sql.countRows("SELECT COUNT(*) FROM notification_token WHERE token = 'token-apns-1' AND provider = 'apns'") == 1);
  CHECK(rootPageOf(database.db) == tableBefore);

  sql.execute("INSERT INTO notification_token (user_id, device_hash, token, provider, public_key, key_id) "
              "VALUES (1, 'device-a', 'token-voip-1', 'apns_voip', 'pk', 'kid-voip')");
  CHECK(sql.lastStatus == SQLITE_OK);
  CHECK(sql.countRows("SELECT COUNT(*) FROM notification_token WHERE user_id = 1 AND device_hash = 'device-a'") == 2);

  sql.execute("INSERT INTO notification_token (user_id, device_hash, token, provider, public_key, key_id) "
              "VALUES (1, 'device-a', 'token-apns-2', 'apns', 'pk', 'kid-apns')");
  CHECK(sql.lastStatus == SQLITE_CONSTRAINT);
}

TEST_CASE("running the migration again changes nothing and loses no row")
{
  OpenDatabase database(kTableWithRow);
  SqliteTokenIndexSql sql(database.db);
  CHECK(notification_token_index::migrate(sql).changed());
  CHECK_FALSE(notification_token_index::migrate(sql).changed());
  CHECK(sql.lastStatus == SQLITE_OK);
  CHECK(sql.countRows("SELECT COUNT(*) FROM notification_token") == 1);
  CHECK(sql.countRows("SELECT COUNT(*) FROM sqlite_master WHERE type = 'index' AND name = 'idx_notification_token_device'") == 1);
}

TEST_CASE("a database without the provider column gets the columns, the device index, keeps its row and holds an apns and an apns_voip row")
{
  OpenDatabase database(kOldestTableWithRow);
  SqliteTokenIndexSql sql(database.db);
  REQUIRE(sql.countRows("SELECT COUNT(*) FROM pragma_table_info('notification_token') WHERE name = 'provider'") == 0);
  REQUIRE(sql.countRows("SELECT COUNT(*) FROM sqlite_master WHERE type = 'index' AND name = 'idx_notification_token_uniq'") == 1);

  const auto result = notification_token_index::migrate(sql);
  CHECK(result.columnsAdded == 4);
  CHECK(result.droppedLegacyIndex);
  CHECK(result.changed());

  CHECK(sql.countRows("SELECT COUNT(*) FROM pragma_table_info('notification_token') WHERE name = 'provider'") == 1);
  CHECK(sql.countRows("SELECT COUNT(*) FROM sqlite_master WHERE type = 'index' AND name = 'idx_notification_token_device'") == 1);
  CHECK(sql.countRows("SELECT COUNT(*) FROM sqlite_master WHERE type = 'index' AND name = 'idx_notification_token_uniq'") == 0);
  CHECK(sql.countRows("SELECT COUNT(*) FROM notification_token WHERE token = 'token-apns-1'") == 1);

  sql.execute("INSERT INTO notification_token (user_id, device_hash, token, provider, public_key, key_id) "
              "VALUES (1, 'device-a', 'token-apns-2', 'apns', 'pk', 'kid-apns')");
  CHECK(sql.lastStatus == SQLITE_OK);
  sql.execute("INSERT INTO notification_token (user_id, device_hash, token, provider, public_key, key_id) "
              "VALUES (1, 'device-a', 'token-voip-2', 'apns_voip', 'pk', 'kid-voip')");
  CHECK(sql.lastStatus == SQLITE_OK);
  CHECK(sql.countRows("SELECT COUNT(*) FROM notification_token WHERE user_id = 1 AND device_hash = 'device-a' "
                      "AND provider IN ('apns', 'apns_voip')") == 2);
}
