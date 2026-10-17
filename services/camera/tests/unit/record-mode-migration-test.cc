#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/camera/services/record-mode-migration.hxx>

#include <filesystem>
#include <memory>
#include <sqlite3.h>
#include <string>
#include <utility>

namespace
{
using DbHandle = std::unique_ptr<sqlite3, int (*)(sqlite3*)>;

constexpr const char* kOldCameraTable =
    "CREATE TABLE camera ("
    "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
    "name TEXT NOT NULL, ip TEXT NOT NULL, "
    "record_mode TEXT NOT NULL DEFAULT 'events' "
    "CHECK (record_mode IN ('events', 'continuous')), "
    "retention_days INTEGER, deleted_at INTEGER)";

constexpr const char* kWidenedCheck =
    "CHECK (record_mode IN ('off', 'motion', 'continuous', 'events'))";

constexpr const char* kWidenedCameraTable =
    "CREATE TABLE camera ("
    "id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
    "name TEXT NOT NULL, ip TEXT NOT NULL, "
    "record_mode TEXT NOT NULL DEFAULT 'events' "
    "CHECK (record_mode IN ('off', 'motion', 'continuous', 'events')), "
    "retention_days INTEGER, deleted_at INTEGER)";

constexpr const char* kChildTables =
    "CREATE TABLE zone (id INTEGER NOT NULL PRIMARY KEY AUTOINCREMENT, "
    "camera_id INTEGER NOT NULL REFERENCES camera(id) ON DELETE CASCADE, "
    "name TEXT NOT NULL DEFAULT ''); "
    "CREATE INDEX idx_zone_camera_id ON zone (camera_id);";

std::string testDirectory()
{
  return (std::filesystem::temp_directory_path() /
          "argus-record-mode-migration-test")
      .string();
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
  const int rc = sqlite3_exec(db, sql.c_str(), nullptr, nullptr, &error);
  const std::string message = error != nullptr ? error : "";
  sqlite3_free(error);
  REQUIRE_MESSAGE(rc == SQLITE_OK, message);
}

std::string textOf(sqlite3* db, const std::string& sql)
{
  sqlite3_stmt* stmt = nullptr;
  REQUIRE(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
  std::string value;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    const auto* text = sqlite3_column_text(stmt, 0);
    if (text != nullptr)
      value = reinterpret_cast<const char*>(text);
  }
  sqlite3_finalize(stmt);
  return value;
}

int64_t countOf(sqlite3* db, const std::string& sql)
{
  sqlite3_stmt* stmt = nullptr;
  REQUIRE(sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_step(stmt) == SQLITE_ROW);
  const int64_t count = sqlite3_column_int64(stmt, 0);
  sqlite3_finalize(stmt);
  return count;
}

std::string definitionOf(sqlite3* db, const std::string& table)
{
  return textOf(db,
                "SELECT sql FROM sqlite_master WHERE type = 'table' AND name = '" +
                    table + "'");
}

struct TempDb
{
  explicit TempDb(const std::string& name)
      : path((std::filesystem::path(testDirectory()) / name).string()),
        backupPrefix(std::filesystem::path(path).filename().string() +
                     ".record-mode-rebuild-")
  {
    std::filesystem::create_directories(testDirectory());
    removeArtifacts();
    db = openFile(path);
    exec(db.get(), "PRAGMA foreign_keys = ON");
  }

  ~TempDb()
  {
    db.reset();
    removeArtifacts();
  }

  void removeArtifacts() const
  {
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    for (const auto& entry :
         std::filesystem::directory_iterator(testDirectory(), ignored)) {
      if (entry.path().filename().string().starts_with(backupPrefix))
        std::filesystem::remove(entry.path(), ignored);
    }
  }

  std::string backupPathFound() const
  {
    std::error_code ignored;
    for (const auto& entry :
         std::filesystem::directory_iterator(testDirectory(), ignored)) {
      if (entry.path().filename().string().starts_with(backupPrefix))
        return entry.path().string();
    }
    return {};
  }

  std::string path;
  std::string backupPrefix;
  DbHandle db{nullptr, sqlite3_close_v2};
};
}

TEST_CASE("the widened check replaces the shipped one and only it")
{
  const std::string narrowed =
      std::string("CREATE TABLE camera (id INTEGER PRIMARY KEY, record_mode TEXT "
                  "NOT NULL DEFAULT 'events' CHECK (record_mode IN ('events', "
                  "'continuous')))");
  const auto widened = record_mode_migration::withWidenedCheck(narrowed);
  if (!widened.has_value())
  {
    FAIL("the shipped check was not widened");
    return;
  }
  CHECK(widened->find(kWidenedCheck) != std::string::npos);
  CHECK(widened->find("('events', 'continuous')") == std::string::npos);
  CHECK(widened->find("record_mode TEXT NOT NULL DEFAULT 'events'") !=
        std::string::npos);

  CHECK_FALSE(record_mode_migration::withWidenedCheck(kWidenedCameraTable)
                  .has_value());
  CHECK_FALSE(record_mode_migration::withWidenedCheck(
                  "CREATE TABLE camera (id INTEGER PRIMARY KEY, record_mode TEXT "
                  "NOT NULL DEFAULT 'events')")
                  .has_value());
  CHECK_FALSE(record_mode_migration::withWidenedCheck("").has_value());
}

TEST_CASE("an already widened table is left alone and no backup is written")
{
  const TempDb fixture("widened.db");
  exec(fixture.db.get(), kWidenedCameraTable);
  exec(fixture.db.get(), "INSERT INTO camera (id, name, ip, record_mode) VALUES "
                         "(1, 'Front', '192.168.1.10', 'motion')");

  const auto outcome = record_mode_migration::apply(
      {.db = fixture.db.get(), .backupPath = fixture.path + ".backup"});
  REQUIRE_MESSAGE(outcome.ok(), outcome.error);
  CHECK(outcome.rebuilt.empty());
  CHECK(outcome.backup.empty());
  CHECK_FALSE(std::filesystem::exists(fixture.path + ".backup"));
  CHECK(fixture.backupPathFound().empty());
  CHECK(textOf(fixture.db.get(), "SELECT record_mode FROM camera WHERE id = 1") ==
        "motion");
}

TEST_CASE("applying on a database with rows keeps them and writes a backup")
{
  TempDb fixture("old.db");
  exec(fixture.db.get(), kOldCameraTable);
  exec(fixture.db.get(), kChildTables);
  exec(fixture.db.get(),
       "INSERT INTO camera (id, name, ip, record_mode, retention_days) VALUES "
       "(1, 'Front', '192.168.1.10', 'events', 7)");
  exec(fixture.db.get(),
       "INSERT INTO camera (id, name, ip, record_mode) VALUES (2, 'Back', "
       "'192.168.1.11', 'continuous')");
  exec(fixture.db.get(),
       "INSERT INTO zone (id, camera_id, name) VALUES (1, 1, 'Door')");
  exec(fixture.db.get(),
       "INSERT INTO zone (id, camera_id, name) VALUES (2, 1, 'Yard')");
  exec(fixture.db.get(), "PRAGMA user_version = 7");
  fixture.db.reset();

  CHECK(record_mode_migration::applyToFile(fixture.path));

  auto rebuilt = openFile(fixture.path);
  CHECK(definitionOf(rebuilt.get(), "camera").find(kWidenedCheck) !=
        std::string::npos);
  CHECK(countOf(rebuilt.get(), "SELECT COUNT(*) FROM camera") == 2);
  CHECK(countOf(rebuilt.get(), "SELECT COUNT(*) FROM zone") == 2);
  CHECK(textOf(rebuilt.get(), "SELECT name FROM camera WHERE id = 1") == "Front");
  CHECK(textOf(rebuilt.get(), "SELECT record_mode FROM camera WHERE id = 1") ==
        "events");
  CHECK(textOf(rebuilt.get(), "SELECT record_mode FROM camera WHERE id = 2") ==
        "continuous");
  CHECK(textOf(rebuilt.get(), "SELECT CAST(retention_days AS TEXT) FROM camera "
                              "WHERE id = 1") == "7");
  CHECK(countOf(rebuilt.get(),
                "SELECT COUNT(*) FROM sqlite_master WHERE type = 'index' AND "
                "name = 'idx_zone_camera_id'") == 1);
  CHECK(textOf(rebuilt.get(), "PRAGMA user_version") == "7");

  exec(rebuilt.get(),
       "INSERT INTO camera (id, name, ip, record_mode) VALUES (3, 'Gate', "
       "'192.168.1.12', 'motion')");
  CHECK(textOf(rebuilt.get(), "SELECT record_mode FROM camera WHERE id = 3") ==
        "motion");
  rebuilt.reset();

  const std::string backup = fixture.backupPathFound();
  REQUIRE_FALSE(backup.empty());
  const auto restored = openFile(backup);
  CHECK(definitionOf(restored.get(), "camera").find("('events', 'continuous')") !=
        std::string::npos);
  CHECK(countOf(restored.get(), "SELECT COUNT(*) FROM camera") == 2);
  CHECK(textOf(restored.get(), "SELECT record_mode FROM camera WHERE id = 2") ==
        "continuous");
}

TEST_CASE("a camera table without a check is never rebuilt")
{
  const TempDb fixture("unchecked.db");
  exec(fixture.db.get(),
       "CREATE TABLE camera (id INTEGER NOT NULL PRIMARY KEY, name TEXT NOT NULL, "
       "ip TEXT NOT NULL, record_mode TEXT NOT NULL DEFAULT 'events')");
  exec(fixture.db.get(),
       "INSERT INTO camera (id, name, ip) VALUES (1, 'Front', '192.168.1.10')");

  const auto outcome = record_mode_migration::apply(
      {.db = fixture.db.get(), .backupPath = fixture.path + ".backup"});
  REQUIRE_MESSAGE(outcome.ok(), outcome.error);
  CHECK(outcome.rebuilt.empty());
  CHECK_FALSE(std::filesystem::exists(fixture.path + ".backup"));
  CHECK(countOf(fixture.db.get(), "SELECT COUNT(*) FROM camera") == 1);
}
