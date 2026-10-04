#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <auth-migration.hxx>

#include <filesystem>
#include <memory>
#include <sqlite3.h>
#include <string>
#include <unistd.h>
#include <utility>
#include <vector>

namespace
{

using DbHandle = std::unique_ptr<sqlite3, int (*)(sqlite3*)>;

struct AuthFixture
{
  std::string sourcePath;
  std::string targetPath;
};

DbHandle openMemory()
{
  sqlite3* raw = nullptr;
  if (sqlite3_open(":memory:", &raw) != SQLITE_OK) {
    sqlite3_close_v2(raw);
    FAIL("could not open an in-memory database");
  }
  return {raw, sqlite3_close_v2};
}

DbHandle openFile(const std::string& path)
{
  sqlite3* raw = nullptr;
  if (sqlite3_open(path.c_str(), &raw) != SQLITE_OK) {
    sqlite3_close_v2(raw);
    FAIL("could not open " << path);
  }
  return {raw, sqlite3_close_v2};
}

DbHandle openUriFile(const std::string& path)
{
  sqlite3* raw = nullptr;
  if (sqlite3_open_v2(path.c_str(), &raw,
                      SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE |
                          SQLITE_OPEN_URI,
                      nullptr) != SQLITE_OK) {
    sqlite3_close_v2(raw);
    FAIL("could not open " << path);
  }
  return {raw, sqlite3_close_v2};
}

void exec(sqlite3* db, const std::string& sql)
{
  char* error = nullptr;
  const bool ok =
      sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error) == SQLITE_OK;
  const std::string message =
      error ? std::string(error) : std::string("exec failed");
  sqlite3_free(error);
  REQUIRE_MESSAGE(ok, message);
}

std::vector<std::string> queryColumn(sqlite3* db, const std::string& sql)
{
  sqlite3_stmt* stmt = nullptr;
  REQUIRE(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
  std::vector<std::string> rows;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const auto* text =
        reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    rows.emplace_back(text ? text : "");
  }
  sqlite3_finalize(stmt);
  return rows;
}

int64_t countOf(sqlite3* db, const std::string& table)
{
  sqlite3_stmt* stmt = nullptr;
  REQUIRE(sqlite3_prepare_v2(db,
                             ("SELECT COUNT(*) FROM \"" + table + "\"")
                                 .c_str(),
                             -1, &stmt, nullptr)
          == SQLITE_OK);
  REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
  const int64_t count = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return count;
}

std::string textOf(sqlite3* db, const std::string& sql)
{
  sqlite3_stmt* stmt = nullptr;
  REQUIRE(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
  const auto* text =
      reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
  const std::string value = text ? text : "";
  sqlite3_finalize(stmt);
  return value;
}

AuthFixture makeFixture()
{
  const auto dir = std::filesystem::temp_directory_path() /
                   ("argus-auth-migration-test-" + std::to_string(::getpid()));
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return AuthFixture{.sourcePath = (dir / "identity.db").string(),
                     .targetPath = (dir / "auth.db").string()};
}

void seedSource(sqlite3* db)
{
  exec(db, "INSERT INTO refresh_token (id, user_id, access_token, "
           "refresh_token, device_hash, user_agent, is_valid, is_used, "
           "expires_at, created_at) VALUES (1, 7, 'access-1', 'refresh-1', "
           "'device-1', 'argus-app/1', 1, 0, 1900000000, 1800000000)");
  exec(db, "INSERT INTO refresh_token (id, user_id, access_token, "
           "refresh_token, device_hash, expires_at) VALUES (2, 8, 'access-2', "
           "'refresh-2', 'device-2', 1900000000)");
  exec(db, "INSERT INTO device_credential (id, user_id, device_hash, "
           "secret_hash, created_at) VALUES (1, 7, 'device-1', 'secret-hash-1', "
           "1800000000)");
  exec(db, "INSERT INTO device_login_challenge (id, challenge_id, device_hash, "
           "status, expires_at) VALUES (1, 'challenge-1', 'device-3', "
           "'pending', 1900000000)");
}

AuthFixture makeSeededSource()
{
  const auto fixture = makeFixture();
  const auto source = openFile(fixture.sourcePath);
  const auto schema = applyAuthSchema(
      {.db = source.get(), .schemaPath = ARGUS_AUTH_SCHEMA_PATH});
  REQUIRE_MESSAGE(schema.ok, schema.error);
  seedSource(source.get());
  return fixture;
}

}

TEST_CASE("auth schema applies cleanly to an in-memory database")
{
  const auto db = openMemory();
  const auto result =
      applyAuthSchema({.db = db.get(), .schemaPath = ARGUS_AUTH_SCHEMA_PATH});
  REQUIRE_MESSAGE(result.ok, result.error);

  const auto tables =
      queryColumn(db.get(),
                  "SELECT name FROM sqlite_master WHERE type = 'table' AND "
                  "name IN ('refresh_token', 'device_login_challenge', "
                  "'device_credential')");
  REQUIRE(tables.size() == 3);

  const auto indexes =
      queryColumn(db.get(),
                  "SELECT name FROM sqlite_master WHERE type = 'index' AND "
                  "name LIKE 'idx_refresh_token%' ORDER BY name");
  CHECK(indexes.size() == 4);
  CHECK(std::ranges::find(indexes, "idx_refresh_token_session") !=
        indexes.end());
}

TEST_CASE("migration copies auth tables into an empty target and verifies them")
{
  const auto fixture = makeSeededSource();

  const auto report = migrateAuth({.sourcePath = fixture.sourcePath,
                                   .targetPath = fixture.targetPath,
                                   .schemaPath = ARGUS_AUTH_SCHEMA_PATH});
  REQUIRE_MESSAGE(report.ok, report.error);
  REQUIRE(report.tables.size() == 3);

  const auto target = openFile(fixture.targetPath);
  CHECK(countOf(target.get(), "refresh_token") == 2);
  CHECK(countOf(target.get(), "device_credential") == 1);
  CHECK(countOf(target.get(), "device_login_challenge") == 1);

  for (const auto& entry : report.tables) {
    CHECK(entry.sourceRows == entry.targetRows);
    CHECK_FALSE(entry.sourceChecksum.empty());
    CHECK(entry.sourceChecksum == entry.targetChecksum);
  }
}

TEST_CASE("a source written before the session columns still migrates")
{
  const auto fixture = makeFixture();
  {
    const auto source = openFile(fixture.sourcePath);
    exec(source.get(),
         "CREATE TABLE refresh_token (id INTEGER NOT NULL PRIMARY KEY "
         "AUTOINCREMENT, user_id INTEGER NOT NULL, access_token TEXT NOT NULL, "
         "refresh_token TEXT NOT NULL, device_hash TEXT NOT NULL, user_agent "
         "TEXT NOT NULL DEFAULT '', is_valid INTEGER NOT NULL DEFAULT 1, "
         "is_used INTEGER NOT NULL DEFAULT 0, expires_at INTEGER NOT NULL, "
         "created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')))");
    exec(source.get(),
         "CREATE TABLE device_login_challenge (id INTEGER NOT NULL PRIMARY KEY "
         "AUTOINCREMENT, challenge_id TEXT NOT NULL UNIQUE, device_hash TEXT "
         "NOT NULL, user_agent TEXT NOT NULL DEFAULT '', status TEXT NOT NULL "
         "DEFAULT 'pending', user_id INTEGER, access_token TEXT, refresh_token "
         "TEXT, expires_at INTEGER NOT NULL, created_at INTEGER NOT NULL "
         "DEFAULT (strftime('%s', 'now')))");
    exec(source.get(),
         "CREATE TABLE device_credential (id INTEGER NOT NULL PRIMARY KEY "
         "AUTOINCREMENT, user_id INTEGER NOT NULL, device_hash TEXT NOT NULL, "
         "secret_hash TEXT NOT NULL UNIQUE, is_active INTEGER NOT NULL DEFAULT "
         "1, created_at INTEGER NOT NULL DEFAULT (strftime('%s', 'now')))");
    seedSource(source.get());
  }

  const auto report = migrateAuth({.sourcePath = fixture.sourcePath,
                                   .targetPath = fixture.targetPath,
                                   .schemaPath = ARGUS_AUTH_SCHEMA_PATH});
  REQUIRE_MESSAGE(report.ok, report.error);

  const auto target = openFile(fixture.targetPath);
  CHECK(countOf(target.get(), "refresh_token") == 2);
  CHECK(textOf(target.get(),
               "SELECT platform FROM refresh_token WHERE id = 1") == "unknown");
  CHECK(textOf(target.get(),
               "SELECT session_id FROM refresh_token WHERE id = 1").empty());
}

TEST_CASE("migration merges beside rows the target already holds")
{
  const auto fixture = makeSeededSource();
  {
    const auto target = openUriFile(fixture.targetPath);
    const auto schema = applyAuthSchema(
        {.db = target.get(), .schemaPath = ARGUS_AUTH_SCHEMA_PATH});
    REQUIRE_MESSAGE(schema.ok, schema.error);
    exec(target.get(),
         "INSERT INTO refresh_token (id, user_id, access_token, refresh_token, "
         "device_hash, expires_at) VALUES (100, 9, 'live-access', "
         "'live-refresh', 'live-device', 1900000000)");
  }

  const auto report = migrateAuth({.sourcePath = fixture.sourcePath,
                                   .targetPath = fixture.targetPath,
                                   .schemaPath = ARGUS_AUTH_SCHEMA_PATH});
  REQUIRE_MESSAGE(report.ok, report.error);

  const auto target = openFile(fixture.targetPath);
  CHECK(countOf(target.get(), "refresh_token") == 3);
  CHECK(textOf(target.get(),
               "SELECT device_hash FROM refresh_token WHERE id = 100")
        == "live-device");
  CHECK(textOf(target.get(),
               "SELECT device_hash FROM refresh_token WHERE id = 1")
        == "device-1");
}

TEST_CASE("migration is idempotent across repeated runs")
{
  const auto fixture = makeSeededSource();

  const auto first = migrateAuth({.sourcePath = fixture.sourcePath,
                                  .targetPath = fixture.targetPath,
                                  .schemaPath = ARGUS_AUTH_SCHEMA_PATH});
  REQUIRE_MESSAGE(first.ok, first.error);

  const auto second = migrateAuth({.sourcePath = fixture.sourcePath,
                                   .targetPath = fixture.targetPath,
                                   .schemaPath = ARGUS_AUTH_SCHEMA_PATH});
  REQUIRE_MESSAGE(second.ok, second.error);

  const auto target = openFile(fixture.targetPath);
  CHECK(countOf(target.get(), "refresh_token") == 2);
  CHECK(countOf(target.get(), "device_credential") == 1);
  CHECK(countOf(target.get(), "device_login_challenge") == 1);
}

TEST_CASE("verification detects a tampered target")
{
  const auto fixture = makeSeededSource();

  const auto report = migrateAuth({.sourcePath = fixture.sourcePath,
                                   .targetPath = fixture.targetPath,
                                   .schemaPath = ARGUS_AUTH_SCHEMA_PATH});
  REQUIRE_MESSAGE(report.ok, report.error);

  const auto target = openUriFile(fixture.targetPath);
  exec(target.get(), "ATTACH DATABASE 'file:" + fixture.sourcePath
                         + "?mode=ro' AS src");
  exec(target.get(),
       "UPDATE refresh_token SET device_hash = 'tampered' WHERE id = 1");
  const auto verification = verifyAuthTables({.target = target.get()});
  CHECK_FALSE(verification.ok);
  CHECK_MESSAGE(verification.error.find("refresh_token") != std::string::npos,
                verification.error);
}

TEST_CASE("migration refuses when source and target are the same file")
{
  const auto fixture = makeSeededSource();
  const auto sizeBefore = std::filesystem::file_size(fixture.sourcePath);

  const auto report = migrateAuth({.sourcePath = fixture.sourcePath,
                                   .targetPath = fixture.sourcePath,
                                   .schemaPath = ARGUS_AUTH_SCHEMA_PATH});
  CHECK_FALSE(report.ok);
  CHECK_MESSAGE(report.error.find("same file") != std::string::npos,
                report.error);
  CHECK(std::filesystem::file_size(fixture.sourcePath) == sizeBefore);

  const auto source = openFile(fixture.sourcePath);
  CHECK(countOf(source.get(), "refresh_token") == 2);
}

TEST_CASE("migration refuses a source without the auth tables")
{
  const auto fixture = makeFixture();
  {
    const auto source = openFile(fixture.sourcePath);
    exec(source.get(), "CREATE TABLE unrelated (id INTEGER PRIMARY KEY)");
  }

  const auto report = migrateAuth({.sourcePath = fixture.sourcePath,
                                   .targetPath = fixture.targetPath,
                                   .schemaPath = ARGUS_AUTH_SCHEMA_PATH});
  CHECK_FALSE(report.ok);
  CHECK_MESSAGE(report.error.find("auth table") != std::string::npos,
                report.error);
  CHECK_FALSE(std::filesystem::exists(fixture.targetPath));
}

TEST_CASE("migration fails when the source database is missing")
{
  const auto fixture = makeFixture();
  const auto report = migrateAuth({.sourcePath = fixture.sourcePath,
                                   .targetPath = fixture.targetPath,
                                   .schemaPath = ARGUS_AUTH_SCHEMA_PATH});
  CHECK_FALSE(report.ok);
  CHECK_FALSE(report.error.empty());
  CHECK_FALSE(std::filesystem::exists(fixture.targetPath));
}
