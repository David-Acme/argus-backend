#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <sync-migration.hxx>

#include <array>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sqlite3.h>
#include <string>
#include <string_view>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{

using DbHandle = std::unique_ptr<sqlite3, int (*)(sqlite3*)>;

constexpr std::array<std::string_view, 5> kTables = {
    "audit_log",
    "user_audit_log",
    "audit_compaction_state",
    "user_action_log",
    "notification_delivery_inbox",
};

constexpr std::string_view kLegacyJournalDdl = R"(
CREATE TABLE user_action_log (
    id         INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,
    user_id    INTEGER NOT NULL,
    record_id  INTEGER NOT NULL,
    table_name TEXT    NOT NULL,
    action     TEXT    NOT NULL,
    old_data   TEXT    NOT NULL  DEFAULT '{}',
    new_data   TEXT    NOT NULL  DEFAULT '{}',
    ip_address TEXT    NOT NULL  DEFAULT '',
    created_at INTEGER NOT NULL  DEFAULT 0,
    msg_id     TEXT    NOT NULL  DEFAULT ''
))";

constexpr std::string_view kLegacyJournalSeed =
    "INSERT INTO user_action_log (id, user_id, record_id, table_name, action, "
    "old_data, new_data, ip_address, created_at, msg_id) VALUES "
    "(1, 4, 42, 'user', 'read', '{}', '{}', '', 1400, 'identity-action:1'), "
    "(2, 4, 43, 'user', 'update', '{\"e\":5}', '{\"e\":6}', '10.0.0.9', "
    "1500, '')";

struct SyncFixture
{
  std::string sourcePath;
  std::string targetPath;
  std::string directory;
};

DbHandle openMemory()
{
  sqlite3* raw = nullptr;
  REQUIRE(sqlite3_open(":memory:", &raw) == SQLITE_OK);
  return {raw, sqlite3_close_v2};
}

DbHandle openFile(const std::string& path)
{
  sqlite3* raw = nullptr;
  REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
  return {raw, sqlite3_close_v2};
}

void exec(sqlite3* db, const std::string& sql)
{
  char* error = nullptr;
  REQUIRE_MESSAGE(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error)
                      == SQLITE_OK,
                  (error ? std::string(error) : std::string("exec failed")));
  sqlite3_free(error);
}

std::vector<std::string> queryColumn(sqlite3* db, const std::string& sql)
{
  sqlite3_stmt* stmt = nullptr;
  REQUIRE(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
  std::vector<std::string> rows;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    rows.emplace_back(text ? text : "");
  }
  sqlite3_finalize(stmt);
  return rows;
}

int64_t scalar(sqlite3* db, const std::string& sql)
{
  sqlite3_stmt* stmt = nullptr;
  REQUIRE(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
  const int64_t value = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return value;
}

int64_t countOf(sqlite3* db, const std::string& table)
{
  return scalar(db, "SELECT COUNT(*) FROM \"" + table + "\"");
}

SyncFixture makeFixture()
{
  const auto dir = std::filesystem::temp_directory_path()
                   / ("argus-sync-migration-test-"
                      + std::to_string(::getpid()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return SyncFixture{.sourcePath = (dir / "identity.db").string(),
                     .targetPath = (dir / "sync.db").string(),
                     .directory = dir.string()};
}

void seedSource(sqlite3* db)
{
  exec(db, "INSERT INTO audit_log (id, create_user_id, record_id, table_name, "
           "changes, priority, event_timestamp, created_at) VALUES "
           "(1, 4, 11, 'camera', '{\"a\":1}', 2, 1000, 1000), "
           "(2, 0, 7, 'user', '{\"b\":2}', 1, 1100, 1100)");
  exec(db, "INSERT INTO user_audit_log (id, user_id, record_id, table_name, "
           "changes, priority, event_timestamp, created_at) VALUES "
           "(1, 4, 7, 'user', '{\"c\":3}', 1, 1200, 1200), "
           "(2, 5, 9, 'person', '{\"d\":4}', 1, 1300, 1300)");
  exec(db, "INSERT INTO audit_compaction_state (table_name, "
           "compacted_through_id) VALUES ('audit_log', 1), "
           "('user_audit_log', 2)");
  exec(db, "INSERT INTO user_action_log (id, user_id, record_id, table_name, "
           "action, old_data, new_data, ip_address, msg_id, created_at) VALUES "
           "(1, 4, 42, 'user', 'read', '{}', '{}', '', 'identity-action:1', "
           "1400), "
           "(2, 4, 43, 'user', 'update', '{\"e\":5}', '{\"e\":6}', "
           "'10.0.0.9', '', 1500)");
  exec(db, "INSERT INTO notification_delivery_inbox (delivery_id, "
           "notification_id, user_id, fingerprint, attempts, status, "
           "created_at, updated_at) VALUES "
           "(5, 3, 4, 'fp-5', 1, 'received', 1600, 1600), "
           "(6, 3, 5, 'fp-6', 2, 'dispatched', 1700, 1700)");
}

void seedSourceFile(const SyncFixture& fixture)
{
  const auto source = openFile(fixture.sourcePath);
  const auto schema = applySyncSchema(
      {.db = source.get(), .schemaPath = ARGUS_SYNC_SCHEMA_PATH});
  REQUIRE_MESSAGE(schema.ok, schema.error);
  exec(source.get(), "CREATE TABLE unrelated (id INTEGER PRIMARY KEY)");
  exec(source.get(), "INSERT INTO unrelated (id) VALUES (7)");
  seedSource(source.get());
}

void checkTargetEmpty(const std::string& targetPath)
{
  const auto target = openFile(targetPath);
  for (const auto table : kTables) {
    CAPTURE(table);
    CHECK(countOf(target.get(), std::string(table)) == 0);
  }
}

}

TEST_CASE("the sync schema carries the five tables without a user reference")
{
  const auto db = openMemory();
  const auto result = applySyncSchema(
      {.db = db.get(), .schemaPath = ARGUS_SYNC_SCHEMA_PATH});
  REQUIRE_MESSAGE(result.ok, result.error);

  const auto tables = queryColumn(db.get(),
      "SELECT name FROM sqlite_master WHERE type = 'table' AND name IN "
      "('audit_log', 'user_audit_log', 'audit_compaction_state', "
      "'user_action_log', 'notification_delivery_inbox') ORDER BY name");
  REQUIRE(tables.size() == 5);

  for (const auto& table : tables) {
    CAPTURE(table);
    CHECK(scalar(db.get(), "SELECT COUNT(*) FROM pragma_foreign_key_list('"
                               + table + "')") == 0);
  }

  const auto indexes = queryColumn(db.get(),
      "SELECT name FROM sqlite_master WHERE type = 'index' AND name LIKE "
      "'idx_%' ORDER BY name");
  CHECK(indexes.size() == 6);
}

TEST_CASE("the migration copies the five tables and verifies every row")
{
  const auto fixture = makeFixture();
  seedSourceFile(fixture);
  const auto sourceSize = std::filesystem::file_size(fixture.sourcePath);

  const auto report = migrateSync({.sourcePath = fixture.sourcePath,
                                   .targetPath = fixture.targetPath,
                                   .schemaPath = ARGUS_SYNC_SCHEMA_PATH});
  REQUIRE_MESSAGE(report.ok, report.error);
  REQUIRE(report.tables.size() == 5);
  CHECK_FALSE(report.sourceReadWrite);

  const auto target = openFile(fixture.targetPath);
  for (const auto table : kTables) {
    CAPTURE(table);
    CHECK(countOf(target.get(), std::string(table)) == 2);
  }
  for (const auto& entry : report.tables) {
    CAPTURE(entry.table);
    CHECK(entry.present);
    CHECK(entry.copiedRows == 2);
    CHECK(entry.skippedRows == 0);
    CHECK_FALSE(entry.sourceChecksum.empty());
    CHECK(entry.sourceChecksum == entry.targetChecksum);
  }

  CHECK(queryColumn(target.get(),
                    "SELECT name FROM sqlite_master WHERE type = 'table' AND "
                    "name = 'unrelated'")
            .empty());
  CHECK(std::filesystem::file_size(fixture.sourcePath) == sourceSize);

  const auto source = openFile(fixture.sourcePath);
  CHECK(countOf(source.get(), "audit_log") == 2);
  CHECK(countOf(source.get(), "user_action_log") == 2);
}

TEST_CASE("the migration keeps the ids the client watermark pages by")
{
  const auto fixture = makeFixture();
  seedSourceFile(fixture);

  const auto report = migrateSync({.sourcePath = fixture.sourcePath,
                                   .targetPath = fixture.targetPath,
                                   .schemaPath = ARGUS_SYNC_SCHEMA_PATH});
  REQUIRE_MESSAGE(report.ok, report.error);

  const auto target = openFile(fixture.targetPath);
  const auto ids = queryColumn(target.get(),
                               "SELECT id FROM audit_log ORDER BY id");
  REQUIRE(ids.size() == 2);
  CHECK(ids[0] == "1");
  CHECK(ids[1] == "2");
  const auto journalIds =
      queryColumn(target.get(), "SELECT id FROM user_action_log ORDER BY id");
  REQUIRE(journalIds.size() == 2);
  CHECK(journalIds[0] == "1");
  CHECK(journalIds[1] == "2");
  CHECK(queryColumn(target.get(),
                    "SELECT msg_id FROM user_action_log WHERE id = 1")
            .front() == "identity-action:1");

  exec(target.get(),
       "INSERT INTO audit_log (record_id, table_name, event_timestamp) VALUES "
       "(12, 'camera', 2000)");
  CHECK(scalar(target.get(), "SELECT max(id) FROM audit_log") == 3);
  exec(target.get(),
       "INSERT INTO user_action_log (user_id, record_id, table_name, action) "
       "VALUES (4, 44, 'user', 'read')");
  CHECK(scalar(target.get(), "SELECT max(id) FROM user_action_log") == 3);
}

TEST_CASE("a second run copies nothing and a pruned row comes back")
{
  const auto fixture = makeFixture();
  seedSourceFile(fixture);

  const auto first = migrateSync({.sourcePath = fixture.sourcePath,
                                  .targetPath = fixture.targetPath,
                                  .schemaPath = ARGUS_SYNC_SCHEMA_PATH});
  REQUIRE_MESSAGE(first.ok, first.error);

  const auto second = migrateSync({.sourcePath = fixture.sourcePath,
                                   .targetPath = fixture.targetPath,
                                   .schemaPath = ARGUS_SYNC_SCHEMA_PATH});
  REQUIRE_MESSAGE(second.ok, second.error);
  for (const auto& entry : second.tables) {
    CAPTURE(entry.table);
    CHECK(entry.present);
    CHECK(entry.copiedRows == 0);
    CHECK(entry.skippedRows == 2);
    CHECK(entry.sourceChecksum == entry.targetChecksum);
  }

  {
    const auto target = openFile(fixture.targetPath);
    exec(target.get(), "DELETE FROM audit_log WHERE id = 1");
  }

  const auto third = migrateSync({.sourcePath = fixture.sourcePath,
                                  .targetPath = fixture.targetPath,
                                  .schemaPath = ARGUS_SYNC_SCHEMA_PATH});
  REQUIRE_MESSAGE(third.ok, third.error);
  REQUIRE(third.tables.size() == 5);
  CHECK(third.tables.front().table == "audit_log");
  CHECK(third.tables.front().copiedRows == 1);
  CHECK(third.tables.front().skippedRows == 1);
  CHECK(third.tables[1].copiedRows == 0);
  CHECK(third.tables[1].skippedRows == 2);

  const auto target = openFile(fixture.targetPath);
  CHECK(countOf(target.get(), "audit_log") == 2);
}

TEST_CASE("a target row that shares a key is kept, not replaced")
{
  const auto fixture = makeFixture();
  seedSourceFile(fixture);
  {
    const auto target = openFile(fixture.targetPath);
    const auto schema = applySyncSchema(
        {.db = target.get(), .schemaPath = ARGUS_SYNC_SCHEMA_PATH});
    REQUIRE_MESSAGE(schema.ok, schema.error);
    exec(target.get(),
         "INSERT INTO audit_log (id, record_id, table_name, changes, "
         "event_timestamp) VALUES (1, 77, 'camera', '{\"own\":1}', 900), "
         "(99, 78, 'camera', '{\"own\":2}', 901)");
  }

  const auto report = migrateSync({.sourcePath = fixture.sourcePath,
                                   .targetPath = fixture.targetPath,
                                   .schemaPath = ARGUS_SYNC_SCHEMA_PATH});
  REQUIRE_MESSAGE(report.ok, report.error);
  REQUIRE(report.tables.size() == 5);

  const auto target = openFile(fixture.targetPath);
  CHECK(countOf(target.get(), "audit_log") == 3);
  CHECK(queryColumn(target.get(),
                    "SELECT changes FROM audit_log WHERE id = 1").front()
        == "{\"own\":1}");
  CHECK(queryColumn(target.get(),
                    "SELECT changes FROM audit_log WHERE id = 2").front()
        == "{\"b\":2}");
  CHECK(report.tables.front().copiedRows == 1);
  CHECK(report.tables.front().skippedRows == 1);
}

TEST_CASE("a journal row the target already holds rolls the whole copy back")
{
  const auto fixture = makeFixture();
  seedSourceFile(fixture);
  {
    const auto target = openFile(fixture.targetPath);
    const auto schema = applySyncSchema(
        {.db = target.get(), .schemaPath = ARGUS_SYNC_SCHEMA_PATH});
    REQUIRE_MESSAGE(schema.ok, schema.error);
    exec(target.get(),
         "INSERT INTO user_action_log (id, user_id, record_id, table_name, "
         "action, msg_id) VALUES (99, 4, 90, 'user', 'read', "
         "'identity-action:1')");
  }

  const auto report = migrateSync({.sourcePath = fixture.sourcePath,
                                   .targetPath = fixture.targetPath,
                                   .schemaPath = ARGUS_SYNC_SCHEMA_PATH});
  CHECK_FALSE(report.ok);
  CHECK_MESSAGE(report.error.find("user_action_log") != std::string::npos,
                report.error);
  CHECK(report.tables.empty());

  const auto target = openFile(fixture.targetPath);
  CHECK(countOf(target.get(), "audit_log") == 0);
  CHECK(countOf(target.get(), "user_audit_log") == 0);
  CHECK(countOf(target.get(), "audit_compaction_state") == 0);
  CHECK(countOf(target.get(), "notification_delivery_inbox") == 0);
  CHECK(countOf(target.get(), "user_action_log") == 1);
  CHECK(queryColumn(target.get(),
                    "SELECT record_id FROM user_action_log WHERE id = 99")
            .front() == "90");
}

TEST_CASE("a source whose journal columns are out of order still migrates")
{
  const auto fixture = makeFixture();
  seedSourceFile(fixture);
  {
    const auto source = openFile(fixture.sourcePath);
    exec(source.get(), "DROP TABLE user_action_log");
    exec(source.get(), std::string(kLegacyJournalDdl));
    exec(source.get(), std::string(kLegacyJournalSeed));
    CHECK(queryColumn(source.get(),
                      "SELECT name FROM pragma_table_info('user_action_log') "
                      "ORDER BY cid")
              .back() == "msg_id");
  }

  const auto report = migrateSync({.sourcePath = fixture.sourcePath,
                                   .targetPath = fixture.targetPath,
                                   .schemaPath = ARGUS_SYNC_SCHEMA_PATH});
  REQUIRE_MESSAGE(report.ok, report.error);

  const auto target = openFile(fixture.targetPath);
  CHECK(countOf(target.get(), "user_action_log") == 2);
  CHECK(queryColumn(target.get(),
                    "SELECT msg_id FROM user_action_log WHERE id = 1").front()
        == "identity-action:1");
  CHECK(scalar(target.get(),
               "SELECT created_at FROM user_action_log WHERE id = 1") == 1400);
  CHECK(queryColumn(target.get(),
                    "SELECT msg_id FROM user_action_log WHERE id = 2").front()
        == "");
  CHECK(scalar(target.get(),
               "SELECT created_at FROM user_action_log WHERE id = 2") == 1500);
}

TEST_CASE("a source with a different column set is refused")
{
  const auto fixture = makeFixture();
  seedSourceFile(fixture);
  {
    const auto source = openFile(fixture.sourcePath);
    exec(source.get(), "DROP TABLE audit_log");
    exec(source.get(),
         "CREATE TABLE audit_log ("
         "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT,"
         "record_id INTEGER NOT NULL,"
         "table_name TEXT NOT NULL,"
         "changes TEXT NOT NULL DEFAULT '{}',"
         "priority INTEGER NOT NULL DEFAULT 1,"
         "event_timestamp INTEGER NOT NULL,"
         "created_at INTEGER NOT NULL DEFAULT 0)");
    exec(source.get(),
         "INSERT INTO audit_log (id, record_id, table_name, event_timestamp) "
         "VALUES (1, 11, 'camera', 1000)");
  }

  const auto report = migrateSync({.sourcePath = fixture.sourcePath,
                                   .targetPath = fixture.targetPath,
                                   .schemaPath = ARGUS_SYNC_SCHEMA_PATH});
  CHECK_FALSE(report.ok);
  CHECK_MESSAGE(report.error.find("audit_log") != std::string::npos,
                report.error);
  CHECK_MESSAGE(report.error.find("create_user_id") != std::string::npos,
                report.error);
  CHECK(report.tables.empty());

  checkTargetEmpty(fixture.targetPath);
}

TEST_CASE("a target schema without one of the tables fails that copy")
{
  const auto fixture = makeFixture();
  seedSourceFile(fixture);
  const auto partial = std::filesystem::path(fixture.directory) / "partial.sql";
  {
    std::ofstream file(partial);
    file << "CREATE TABLE IF NOT EXISTS audit_log (\n"
            "    id              INTEGER NOT NULL  PRIMARY KEY AUTOINCREMENT,\n"
            "    create_user_id  INTEGER,\n"
            "    record_id       INTEGER NOT NULL,\n"
            "    table_name      TEXT    NOT NULL,\n"
            "    changes         TEXT    NOT NULL  DEFAULT '{}',\n"
            "    priority        INTEGER NOT NULL  DEFAULT 1,\n"
            "    event_timestamp INTEGER NOT NULL,\n"
            "    created_at      INTEGER NOT NULL  DEFAULT (strftime('%s', 'now'))\n"
            ");\n";
  }

  const auto report = migrateSync({.sourcePath = fixture.sourcePath,
                                   .targetPath = fixture.targetPath,
                                   .schemaPath = partial.string()});
  CHECK_FALSE(report.ok);
  CHECK_MESSAGE(report.error.find("user_audit_log") != std::string::npos,
                report.error);

  const auto target = openFile(fixture.targetPath);
  CHECK(countOf(target.get(), "audit_log") == 0);
}

TEST_CASE("the migration refuses a source without the audit tables")
{
  const auto fixture = makeFixture();
  {
    const auto source = openFile(fixture.sourcePath);
    exec(source.get(), "CREATE TABLE unrelated (id INTEGER PRIMARY KEY)");
  }

  const auto report = migrateSync({.sourcePath = fixture.sourcePath,
                                   .targetPath = fixture.targetPath,
                                   .schemaPath = ARGUS_SYNC_SCHEMA_PATH});
  REQUIRE_MESSAGE(report.ok, report.error);
  REQUIRE(report.tables.size() == 5);
  for (const auto& entry : report.tables) {
    CAPTURE(entry.table);
    CHECK_FALSE(entry.present);
    CHECK(entry.copiedRows == 0);
    CHECK(entry.skippedRows == 0);
  }

  checkTargetEmpty(fixture.targetPath);
}

TEST_CASE("the migration refuses a source that is the target")
{
  const auto fixture = makeFixture();
  seedSourceFile(fixture);
  const auto sizeBefore = std::filesystem::file_size(fixture.sourcePath);

  const auto report = migrateSync({.sourcePath = fixture.sourcePath,
                                   .targetPath = fixture.sourcePath,
                                   .schemaPath = ARGUS_SYNC_SCHEMA_PATH});
  CHECK_FALSE(report.ok);
  CHECK_MESSAGE(report.error.find("same file") != std::string::npos,
                report.error);
  CHECK(std::filesystem::file_size(fixture.sourcePath) == sizeBefore);

  const auto source = openFile(fixture.sourcePath);
  CHECK(countOf(source.get(), "audit_log") == 2);

  const auto link = std::filesystem::path(fixture.directory) / "linked.db";
  std::filesystem::create_hard_link(fixture.sourcePath, link);
  const auto linked = migrateSync({.sourcePath = fixture.sourcePath,
                                   .targetPath = link.string(),
                                   .schemaPath = ARGUS_SYNC_SCHEMA_PATH});
  CHECK_FALSE(linked.ok);
  CHECK_MESSAGE(linked.error.find("same file") != std::string::npos,
                linked.error);
  CHECK(countOf(source.get(), "audit_log") == 2);
}

TEST_CASE("the migration refuses a source that does not exist")
{
  const auto fixture = makeFixture();
  const auto report = migrateSync(
      {.sourcePath = (std::filesystem::path(fixture.directory) / "absent.db")
                         .string(),
       .targetPath = fixture.targetPath,
       .schemaPath = ARGUS_SYNC_SCHEMA_PATH});
  CHECK_FALSE(report.ok);
  CHECK_MESSAGE(report.error.find("not found") != std::string::npos,
                report.error);
}
