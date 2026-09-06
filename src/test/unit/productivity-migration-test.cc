#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <productivity-migration.hxx>

#include <filesystem>
#include <memory>
#include <sqlite3.h>
#include <string>
#include <utility>
#include <vector>

namespace
{

using DbHandle = std::unique_ptr<sqlite3, int (*)(sqlite3*)>;

struct ProductivityFixture
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

ProductivityFixture makeFixture()
{
  const auto dir = std::filesystem::temp_directory_path()
                   / "argus-productivity-migration-test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return ProductivityFixture{.sourcePath = (dir / "source.db").string(),
                             .targetPath = (dir / "productivity.db").string()};
}

void seedSource(sqlite3* db)
{
  exec(db, "INSERT INTO project (id, owner_id, name) VALUES (1, 1, 'Home')");
  exec(db, "INSERT INTO project (id, owner_id, name, status) "
           "VALUES (2, 2, 'Work', 'done')");
  exec(db, "INSERT INTO project_task (id, project_id, title) "
           "VALUES (1, 1, 'Wire the shelf')");
  exec(db, "INSERT INTO calendar_event (id, owner_id, title, starts_at) "
           "VALUES (1, 1, 'Standup', 1700000000)");
  exec(db, "INSERT INTO reminder (id, target_user_id, title, scheduled_at) "
           "VALUES (1, 1, 'Trash day', 1700000000)");
  exec(db, "INSERT INTO project_member (id, project_id, user_id, access) "
           "VALUES (1, 1, 2, 'view')");
  exec(db, "INSERT INTO calendar_event_share (id, calendar_event_id, user_id) "
           "VALUES (1, 1, 2)");
  exec(db, "INSERT INTO reminder_detail (id, reminder_id, content) "
           "VALUES (1, 1, 'Take out the recycling')");
}

ProductivityMigrationReport migrateSeeded(const ProductivityFixture& fixture)
{
  {
    const auto source = openFile(fixture.sourcePath);
    const auto schema = applyProductivitySchema(
        {.db = source.get(), .schemaPath = ARGUS_PRODUCTIVITY_SCHEMA_PATH});
    REQUIRE_MESSAGE(schema.ok, schema.error);
    seedSource(source.get());
  }
  return migrateProductivity({.sourcePath = fixture.sourcePath,
                              .targetPath = fixture.targetPath,
                              .schemaPath = ARGUS_PRODUCTIVITY_SCHEMA_PATH});
}

} // namespace

TEST_CASE("migration copies the productivity tables and verifies them")
{
  const auto fixture = makeFixture();
  const auto report = migrateSeeded(fixture);
  REQUIRE_MESSAGE(report.ok, report.error);
  REQUIRE_FALSE(report.noop);
  REQUIRE(report.tables.size() == 7);

  const std::vector<std::pair<std::string, int64_t>> expectedCounts = {
      std::pair<std::string, int64_t>{"project", 2},
      std::pair<std::string, int64_t>{"project_task", 1},
      std::pair<std::string, int64_t>{"project_member", 1},
      std::pair<std::string, int64_t>{"calendar_event", 1},
      std::pair<std::string, int64_t>{"calendar_event_share", 1},
      std::pair<std::string, int64_t>{"reminder", 1},
      std::pair<std::string, int64_t>{"reminder_detail", 1},
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
  // FK integrity: the copy keeps rows pointing at real productivity parents;
  // only user references are expected to dangle (the user rows live in
  // identity.db).
  sqlite3_stmt* violations = nullptr;
  REQUIRE(sqlite3_prepare_v2(target.get(), "PRAGMA foreign_key_check", -1,
                             &violations, nullptr) == SQLITE_OK);
  while (sqlite3_step(violations) == SQLITE_ROW) {
    const auto* parent =
        reinterpret_cast<const char*>(sqlite3_column_text(violations, 2));
    const bool isUser = parent != nullptr && std::string(parent) == "user";
    const std::string detail =
        "unexpected foreign_key_check violation against "
        + std::string(parent ? parent : "(null)");
    REQUIRE_MESSAGE(isUser, detail);
  }
  sqlite3_finalize(violations);
}

TEST_CASE("migration of a schema-current target is a verified no-op")
{
  const auto fixture = makeFixture();
  const auto first = migrateSeeded(fixture);
  REQUIRE_MESSAGE(first.ok, first.error);

  const auto second = migrateProductivity(
      {.sourcePath = fixture.sourcePath,
       .targetPath = fixture.targetPath,
       .schemaPath = ARGUS_PRODUCTIVITY_SCHEMA_PATH});
  REQUIRE_MESSAGE(second.ok, second.error);
  CHECK(second.noop);
  CHECK(second.tables.size() == 7);
  for (const auto& entry : second.tables) {
    CHECK(entry.sourceRows == 0);
    CHECK(entry.sourceChecksum.empty());
    CHECK_FALSE(entry.targetChecksum.empty());
  }

  const auto target = openFile(fixture.targetPath);
  CHECK(countOf(target.get(), "project") == 2);
  CHECK(countOf(target.get(), "project_task") == 1);
  CHECK(countOf(target.get(), "reminder_detail") == 1);
}

TEST_CASE("no-op migration does not need the source database")
{
  const auto fixture = makeFixture();
  const auto first = migrateSeeded(fixture);
  REQUIRE_MESSAGE(first.ok, first.error);

  std::filesystem::remove(fixture.sourcePath);
  const auto second = migrateProductivity(
      {.sourcePath = fixture.sourcePath,
       .targetPath = fixture.targetPath,
       .schemaPath = ARGUS_PRODUCTIVITY_SCHEMA_PATH});
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
    // All seven productivity tables exist but project lacks the schema's
    // columns.
    for (const auto* table : {"project", "calendar_event", "reminder",
                              "project_task", "project_member",
                              "calendar_event_share", "reminder_detail"})
      exec(target.get(),
           "CREATE TABLE " + std::string(table) + " (id INTEGER PRIMARY KEY)");
  }

  const auto report = migrateSeeded(fixture);
  CHECK_FALSE(report.ok);
  CHECK_MESSAGE(report.error.find("column shape") != std::string::npos,
                report.error);

  const auto target = openFile(fixture.targetPath);
  CHECK(countOf(target.get(), "project") == 0);
}

TEST_CASE("verification detects a tampered target")
{
  const auto fixture = makeFixture();
  const auto seeded = migrateSeeded(fixture);
  REQUIRE_MESSAGE(seeded.ok, seeded.error);

  const auto target = openUriFile(fixture.targetPath);
  exec(target.get(), "ATTACH DATABASE '" + fixture.sourcePath + "' AS src");
  exec(target.get(), "INSERT INTO project (owner_id, name) "
                     "VALUES (9, 'intruder')");
  const auto verification = verifyProductivityTables({.target = target.get()});
  CHECK_FALSE(verification.ok);
  CHECK_MESSAGE(verification.error.find("project") != std::string::npos,
                verification.error);
}

TEST_CASE("migration refuses when source and target are the same file")
{
  const auto fixture = makeFixture();
  {
    const auto source = openFile(fixture.sourcePath);
    const auto schema = applyProductivitySchema(
        {.db = source.get(), .schemaPath = ARGUS_PRODUCTIVITY_SCHEMA_PATH});
    REQUIRE_MESSAGE(schema.ok, schema.error);
    seedSource(source.get());
  }

  const auto sizeBefore = std::filesystem::file_size(fixture.sourcePath);
  const auto report = migrateProductivity(
      {.sourcePath = fixture.sourcePath,
       .targetPath = fixture.sourcePath,
       .schemaPath = ARGUS_PRODUCTIVITY_SCHEMA_PATH});
  CHECK_FALSE(report.ok);
  CHECK_MESSAGE(report.error.find("same file") != std::string::npos,
                report.error);
  CHECK(std::filesystem::file_size(fixture.sourcePath) == sizeBefore);

  const auto source = openFile(fixture.sourcePath);
  CHECK(countOf(source.get(), "project") == 2);
}

TEST_CASE("migration refuses a source without the productivity tables")
{
  const auto fixture = makeFixture();
  {
    const auto source = openFile(fixture.sourcePath);
    exec(source.get(), "CREATE TABLE unrelated (id INTEGER PRIMARY KEY)");
  }

  const auto report = migrateProductivity(
      {.sourcePath = fixture.sourcePath,
       .targetPath = fixture.targetPath,
       .schemaPath = ARGUS_PRODUCTIVITY_SCHEMA_PATH});
  CHECK_FALSE(report.ok);
  CHECK_MESSAGE(report.error.find("productivity table") != std::string::npos,
                report.error);
  CHECK_FALSE(std::filesystem::exists(fixture.targetPath));
}

TEST_CASE("migration fails when the source database is missing")
{
  const auto fixture = makeFixture();
  const auto report = migrateProductivity(
      {.sourcePath = fixture.sourcePath,
       .targetPath = fixture.targetPath,
       .schemaPath = ARGUS_PRODUCTIVITY_SCHEMA_PATH});
  CHECK_FALSE(report.ok);
  CHECK_MESSAGE(report.error.find("not found") != std::string::npos,
                report.error);
  CHECK_FALSE(std::filesystem::exists(fixture.targetPath));
}