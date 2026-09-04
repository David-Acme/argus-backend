#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <identity-migration.hxx>

#include <filesystem>
#include <memory>
#include <sqlite3.h>
#include <string>
#include <utility>
#include <vector>

namespace
{

using DbHandle = std::unique_ptr<sqlite3, int (*)(sqlite3*)>;

struct IdentityFixture
{
  std::string sourcePath;
  std::string targetPath;
};

DbHandle openMemory()
{
  sqlite3* raw = nullptr;
  REQUIRE(sqlite3_open(":memory:", &raw) == SQLITE_OK);
  return DbHandle(raw, sqlite3_close_v2);
}

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

std::vector<std::string> queryColumn(sqlite3* db, const std::string& sql)
{
  sqlite3_stmt* stmt = nullptr;
  REQUIRE(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
  std::vector<std::string> rows;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const auto* text = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    rows.push_back(text ? text : "");
  }
  sqlite3_finalize(stmt);
  return rows;
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

IdentityFixture makeFixture()
{
  const auto dir = std::filesystem::temp_directory_path()
                   / "argus-identity-migration-test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return IdentityFixture{.sourcePath = (dir / "source.db").string(),
                         .targetPath = (dir / "identity.db").string()};
}

void seedSource(sqlite3* db)
{
  exec(db, "INSERT INTO user (id, name, last_name, role, lang, is_active) "
           "VALUES (1, 'Ada', 'Lovelace', 'owner', 'en', 1)");
  exec(db, "INSERT INTO user (id, name, last_name, role) "
           "VALUES (2, 'Grace', 'Hopper', 'guard')");
  exec(db, "INSERT INTO person (id, user_id, name) VALUES (1, 1, 'Ada')");
  exec(db, "INSERT INTO face_embedding (id, person_id, embedding) "
           "VALUES (1, 1, x'00112233')");
  exec(db, "INSERT INTO refresh_token (id, user_id, access_token, refresh_token, "
           "device_hash, expires_at) VALUES (1, 1, 'a', 'r', 'hash', 100)");
  exec(db, "INSERT INTO device_login_challenge (id, challenge_id, device_hash, "
           "expires_at) VALUES (1, 'challenge-1', 'hash', 100)");
  exec(db, "INSERT INTO user_invitation (id, token_hash, role, max_redemptions, "
           "expires_at, created_by) VALUES (1, 'hash-1', 'guard', 1, 100, 1)");
  exec(db, "INSERT INTO invitation_redemption (id, invitation_id, user_id) "
           "VALUES (1, 1, 2)");
}

} // namespace

TEST_CASE("identity schema applies cleanly to an in-memory database")
{
  const auto db = openMemory();
  const auto result = applyIdentitySchema(
      {.db = db.get(), .schemaPath = ARGUS_IDENTITY_SCHEMA_PATH});
  REQUIRE_MESSAGE(result.ok, result.error);

  const auto tables = queryColumn(db.get(),
      "SELECT name FROM sqlite_master WHERE type = 'table' AND name IN "
      "('user', 'person', 'face_embedding', 'refresh_token', "
      "'device_login_challenge', 'user_invitation', 'invitation_redemption')");
  REQUIRE(tables.size() == 7);

  const auto indexes = queryColumn(db.get(),
      "SELECT name FROM sqlite_master WHERE type = 'index' AND name LIKE "
      "'idx_%' ORDER BY name");
  CHECK(indexes.size() == 19);
}

TEST_CASE("migration copies identity tables and verifies them")
{
  const auto fixture = makeFixture();
  {
    const auto source = openFile(fixture.sourcePath);
    const auto schema = applyIdentitySchema(
        {.db = source.get(), .schemaPath = ARGUS_IDENTITY_SCHEMA_PATH});
    REQUIRE_MESSAGE(schema.ok, schema.error);
    seedSource(source.get());
  }

  const auto report = migrateIdentity({.sourcePath = fixture.sourcePath,
                                       .targetPath = fixture.targetPath,
                                       .schemaPath = ARGUS_IDENTITY_SCHEMA_PATH});
  REQUIRE_MESSAGE(report.ok, report.error);
  REQUIRE(report.tables.size() == 7);

  const std::vector<std::pair<std::string, int64_t>> expectedCounts = {
      std::pair<std::string, int64_t>{"user", 2},
      std::pair<std::string, int64_t>{"person", 1},
      std::pair<std::string, int64_t>{"face_embedding", 1},
      std::pair<std::string, int64_t>{"refresh_token", 1},
      std::pair<std::string, int64_t>{"device_login_challenge", 1},
      std::pair<std::string, int64_t>{"user_invitation", 1},
      std::pair<std::string, int64_t>{"invitation_redemption", 1},
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
}

TEST_CASE("verification detects a tampered target")
{
  const auto fixture = makeFixture();
  {
    const auto source = openFile(fixture.sourcePath);
    const auto schema = applyIdentitySchema(
        {.db = source.get(), .schemaPath = ARGUS_IDENTITY_SCHEMA_PATH});
    REQUIRE_MESSAGE(schema.ok, schema.error);
    seedSource(source.get());
  }

  const auto report = migrateIdentity({.sourcePath = fixture.sourcePath,
                                       .targetPath = fixture.targetPath,
                                       .schemaPath = ARGUS_IDENTITY_SCHEMA_PATH});
  REQUIRE_MESSAGE(report.ok, report.error);

  const auto target = openUriFile(fixture.targetPath);
  exec(target.get(), "ATTACH DATABASE '" + fixture.sourcePath + "' AS src");
  exec(target.get(), "INSERT INTO person (name) VALUES ('intruder')");
  const auto verification = verifyIdentityTables({.target = target.get()});
  CHECK_FALSE(verification.ok);
  CHECK_MESSAGE(verification.error.find("person") != std::string::npos,
                verification.error);
}

TEST_CASE("migration refuses when source and target are the same file")
{
  const auto fixture = makeFixture();
  {
    const auto source = openFile(fixture.sourcePath);
    const auto schema = applyIdentitySchema(
        {.db = source.get(), .schemaPath = ARGUS_IDENTITY_SCHEMA_PATH});
    REQUIRE_MESSAGE(schema.ok, schema.error);
    seedSource(source.get());
  }

  const auto sizeBefore = std::filesystem::file_size(fixture.sourcePath);
  const auto report = migrateIdentity({.sourcePath = fixture.sourcePath,
                                       .targetPath = fixture.sourcePath,
                                       .schemaPath = ARGUS_IDENTITY_SCHEMA_PATH});
  CHECK_FALSE(report.ok);
  CHECK_MESSAGE(report.error.find("same file") != std::string::npos,
                report.error);
  CHECK(std::filesystem::file_size(fixture.sourcePath) == sizeBefore);

  const auto source = openFile(fixture.sourcePath);
  CHECK(countOf(source.get(), "user") == 2);
}

TEST_CASE("migration refuses a source without the identity tables")
{
  const auto fixture = makeFixture();
  {
    const auto source = openFile(fixture.sourcePath);
    exec(source.get(), "CREATE TABLE unrelated (id INTEGER PRIMARY KEY)");
  }

  const auto report = migrateIdentity({.sourcePath = fixture.sourcePath,
                                       .targetPath = fixture.targetPath,
                                       .schemaPath = ARGUS_IDENTITY_SCHEMA_PATH});
  CHECK_FALSE(report.ok);
  CHECK_MESSAGE(report.error.find("identity table") != std::string::npos,
                report.error);
  CHECK_FALSE(std::filesystem::exists(fixture.targetPath));
}

TEST_CASE("migration fails when the source database is missing")
{
  const auto fixture = makeFixture();
  const auto report = migrateIdentity({.sourcePath = fixture.sourcePath,
                                       .targetPath = fixture.targetPath,
                                       .schemaPath = ARGUS_IDENTITY_SCHEMA_PATH});
  CHECK_FALSE(report.ok);
  CHECK_FALSE(report.error.empty());
  CHECK_FALSE(std::filesystem::exists(fixture.targetPath));
}