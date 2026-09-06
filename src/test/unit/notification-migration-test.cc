#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <notification-migration.hxx>

#include <filesystem>
#include <memory>
#include <sqlite3.h>
#include <string>
#include <utility>
#include <vector>

namespace
{

using DbHandle = std::unique_ptr<sqlite3, int (*)(sqlite3*)>;

struct NotificationFixture
{
  std::string sourcePath;
  std::string targetPath;
};

DbHandle openFile(const std::string& path)
{
  sqlite3* raw = nullptr;
  REQUIRE(sqlite3_open(path.c_str(), &raw) == SQLITE_OK);
  return DbHandle(raw, sqlite3_close_v2);
}

DbHandle openUriFile(const std::string& path)
{
  sqlite3* raw = nullptr;
  REQUIRE(sqlite3_open_v2(path.c_str(), &raw, SQLITE_OPEN_READWRITE | SQLITE_OPEN_URI,
                          nullptr) == SQLITE_OK);
  return DbHandle(raw, sqlite3_close_v2);
}

void exec(sqlite3* db, const std::string& sql)
{
  char* error = nullptr;
  REQUIRE_MESSAGE(sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error)
                      == SQLITE_OK,
                  (error ? std::string(error) : std::string("exec failed")));
  sqlite3_free(error);
}

int64_t countOf(sqlite3* db, const std::string& table)
{
  sqlite3_stmt* stmt = nullptr;
  REQUIRE(sqlite3_prepare_v2(db, ("SELECT COUNT(*) FROM \"" + table + "\"").c_str(),
                             -1, &stmt, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
  const int64_t count = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return count;
}

NotificationFixture makeFixture()
{
  const auto dir = std::filesystem::temp_directory_path()
                   / "argus-notification-migration-test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return NotificationFixture{.sourcePath = (dir / "source.db").string(),
                             .targetPath = (dir / "notification.db").string()};
}

void seedSource(sqlite3* db)
{
  exec(db, "INSERT INTO notification (id, user_id, title) "
           "VALUES (1, 1, 'Doorbell')");
  exec(db, "INSERT INTO notification (id, user_id, title, is_read, read_at) "
           "VALUES (2, 2, 'Seen', 1, 1700000000)");
  exec(db, "INSERT INTO notification_token (id, user_id, token, platform) "
           "VALUES (1, 1, 'tok-1', 'android')");
  exec(db, "INSERT INTO notification_token (id, user_id, token, platform, "
           "is_active) VALUES (2, 2, 'tok-2', 'ios', 0)");
}

NotificationMigrationReport migrateSeeded(const NotificationFixture& fixture)
{
  {
    const auto source = openFile(fixture.sourcePath);
    const auto schema = applyNotificationSchema(
        {.db = source.get(), .schemaPath = ARGUS_NOTIFICATION_SCHEMA_PATH});
    REQUIRE_MESSAGE(schema.ok, schema.error);
    seedSource(source.get());
  }
  return migrateNotification({.sourcePath = fixture.sourcePath,
                              .targetPath = fixture.targetPath,
                              .schemaPath = ARGUS_NOTIFICATION_SCHEMA_PATH});
}

} // namespace

TEST_CASE("migration copies the notification tables and verifies them")
{
  const auto fixture = makeFixture();
  const auto report = migrateSeeded(fixture);
  REQUIRE_MESSAGE(report.ok, report.error);
  REQUIRE_FALSE(report.noop);
  REQUIRE(report.tables.size() == 2);

  const std::vector<std::pair<std::string, int64_t>> expectedCounts = {
      std::pair<std::string, int64_t>{"notification", 2},
      std::pair<std::string, int64_t>{"notification_token", 2},
  };

  const auto target = openFile(fixture.targetPath);
  for (const auto& [table, rows] : expectedCounts) {
    CAPTURE(table);
    CHECK(countOf(target.get(), table) == rows);
  }
  for (const auto& entry : report.tables) {
    CHECK(entry.sourceRows == entry.targetRows);
    CHECK_FALSE(entry.sourceChecksum.empty());
    CHECK(entry.sourceChecksum == entry.targetChecksum);
  }
  // FK integrity: only user references are expected to dangle (the user rows
  // live in identity.db).
  sqlite3_stmt* violations = nullptr;
  REQUIRE(sqlite3_prepare_v2(target.get(), "PRAGMA foreign_key_check", -1,
                             &violations, nullptr) == SQLITE_OK);
  while (sqlite3_step(violations) == SQLITE_ROW) {
    const auto* parent =
        reinterpret_cast<const char*>(sqlite3_column_text(violations, 2));
    REQUIRE_MESSAGE(parent != nullptr && std::string(parent) == "user",
                    "unexpected foreign_key_check violation against "
                        + std::string(parent ? parent : "(null)"));
  }
  sqlite3_finalize(violations);
}

TEST_CASE("migration of a schema-current target is a verified no-op")
{
  const auto fixture = makeFixture();
  const auto first = migrateSeeded(fixture);
  REQUIRE_MESSAGE(first.ok, first.error);

  const auto second = migrateNotification(
      {.sourcePath = fixture.sourcePath,
       .targetPath = fixture.targetPath,
       .schemaPath = ARGUS_NOTIFICATION_SCHEMA_PATH});
  REQUIRE_MESSAGE(second.ok, second.error);
  CHECK(second.noop);
  CHECK(second.tables.size() == 2);
  for (const auto& entry : second.tables) {
    CHECK(entry.sourceRows == 0);
    CHECK(entry.sourceChecksum.empty());
    CHECK_FALSE(entry.targetChecksum.empty());
  }

  const auto target = openFile(fixture.targetPath);
  CHECK(countOf(target.get(), "notification") == 2);
  CHECK(countOf(target.get(), "notification_token") == 2);
}

TEST_CASE("no-op migration does not need the source database")
{
  const auto fixture = makeFixture();
  const auto first = migrateSeeded(fixture);
  REQUIRE_MESSAGE(first.ok, first.error);

  std::filesystem::remove(fixture.sourcePath);
  const auto second = migrateNotification(
      {.sourcePath = fixture.sourcePath,
       .targetPath = fixture.targetPath,
       .schemaPath = ARGUS_NOTIFICATION_SCHEMA_PATH});
  REQUIRE_MESSAGE(second.ok, second.error);
  CHECK(second.noop);
}

TEST_CASE("migration refuses a non-schema-current existing target")
{
  const auto fixture = makeFixture();
  {
    const auto target = openFile(fixture.targetPath);
    exec(target.get(), "CREATE TABLE unrelated (id INTEGER PRIMARY KEY)");
  }

  const auto report = migrateSeeded(fixture);
  CHECK_FALSE(report.ok);
  CHECK_MESSAGE(report.error.find("schema-current") != std::string::npos,
                report.error);
  // The operator file is never deleted: the target keeps its own tables.
  const auto target = openFile(fixture.targetPath);
  exec(target.get(), "SELECT * FROM unrelated");
}

TEST_CASE("migration refuses an existing target with a stale column shape")
{
  const auto fixture = makeFixture();
  {
    const auto target = openFile(fixture.targetPath);
    // Both notification tables exist but notification lacks the schema's
    // columns.
    exec(target.get(), "CREATE TABLE notification (id INTEGER PRIMARY KEY)");
    exec(target.get(),
         "CREATE TABLE notification_token (id INTEGER PRIMARY KEY)");
  }

  const auto report = migrateSeeded(fixture);
  CHECK_FALSE(report.ok);
  CHECK_MESSAGE(report.error.find("column shape") != std::string::npos,
                report.error);

  const auto target = openFile(fixture.targetPath);
  CHECK(countOf(target.get(), "notification") == 0);
}

TEST_CASE("verification detects a tampered target")
{
  const auto fixture = makeFixture();
  const auto seeded = migrateSeeded(fixture);
  REQUIRE_MESSAGE(seeded.ok, seeded.error);

  const auto target = openUriFile(fixture.targetPath);
  exec(target.get(), "ATTACH DATABASE '" + fixture.sourcePath + "' AS src");
  exec(target.get(), "INSERT INTO notification (user_id, title) "
                     "VALUES (9, 'intruder')");
  const auto verification = verifyNotificationTables({.target = target.get()});
  CHECK_FALSE(verification.ok);
  CHECK_MESSAGE(verification.error.find("notification") != std::string::npos,
                verification.error);
}

TEST_CASE("migration refuses when source and target are the same file")
{
  const auto fixture = makeFixture();
  {
    const auto source = openFile(fixture.sourcePath);
    const auto schema = applyNotificationSchema(
        {.db = source.get(), .schemaPath = ARGUS_NOTIFICATION_SCHEMA_PATH});
    REQUIRE_MESSAGE(schema.ok, schema.error);
    seedSource(source.get());
  }

  const auto sizeBefore = std::filesystem::file_size(fixture.sourcePath);
  const auto report = migrateNotification(
      {.sourcePath = fixture.sourcePath,
       .targetPath = fixture.sourcePath,
       .schemaPath = ARGUS_NOTIFICATION_SCHEMA_PATH});
  CHECK_FALSE(report.ok);
  CHECK_MESSAGE(report.error.find("same file") != std::string::npos,
                report.error);
  CHECK(std::filesystem::file_size(fixture.sourcePath) == sizeBefore);

  const auto source = openFile(fixture.sourcePath);
  CHECK(countOf(source.get(), "notification") == 2);
}

TEST_CASE("migration refuses a source without the notification tables")
{
  const auto fixture = makeFixture();
  {
    const auto source = openFile(fixture.sourcePath);
    exec(source.get(), "CREATE TABLE unrelated (id INTEGER PRIMARY KEY)");
  }

  const auto report = migrateNotification(
      {.sourcePath = fixture.sourcePath,
       .targetPath = fixture.targetPath,
       .schemaPath = ARGUS_NOTIFICATION_SCHEMA_PATH});
  CHECK_FALSE(report.ok);
  CHECK_MESSAGE(report.error.find("notification table") != std::string::npos,
                report.error);
  CHECK_FALSE(std::filesystem::exists(fixture.targetPath));
}

TEST_CASE("migration fails when the source database is missing")
{
  const auto fixture = makeFixture();
  const auto report = migrateNotification(
      {.sourcePath = fixture.sourcePath,
       .targetPath = fixture.targetPath,
       .schemaPath = ARGUS_NOTIFICATION_SCHEMA_PATH});
  CHECK_FALSE(report.ok);
  CHECK_MESSAGE(report.error.find("not found") != std::string::npos,
                report.error);
  CHECK_FALSE(std::filesystem::exists(fixture.targetPath));
}