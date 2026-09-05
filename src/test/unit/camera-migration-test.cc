#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <camera-migration.hxx>

#include <filesystem>
#include <memory>
#include <sqlite3.h>
#include <string>
#include <utility>
#include <vector>

namespace
{

using DbHandle = std::unique_ptr<sqlite3, int (*)(sqlite3*)>;

struct CameraFixture
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

CameraFixture makeFixture()
{
  const auto dir = std::filesystem::temp_directory_path()
                   / "argus-camera-migration-test";
  std::filesystem::remove_all(dir);
  std::filesystem::create_directories(dir);
  return CameraFixture{.sourcePath = (dir / "source.db").string(),
                       .targetPath = (dir / "camera.db").string()};
}

void seedSource(sqlite3* db)
{
  exec(db, "INSERT INTO camera (id, name, manufacturer, model, ip, driver, "
           "record_mode, retention_days) "
           "VALUES (1, 'Front', 'Tapo', 'C225', '192.168.1.10', 'tapo', "
           "'events', 7)");
  exec(db, "INSERT INTO camera (id, name, ip, driver) "
           "VALUES (2, 'Back', '192.168.1.11', 'onvif')");
  exec(db, "INSERT INTO camera_stream (id, camera_id, label, url, resolution, "
           "fps, codec, is_primary) "
           "VALUES (1, 1, 'main', 'rtsp://192.168.1.10/stream1', "
           "'2560x1440', 30, 'h264', 1)");
  exec(db, "INSERT INTO camera_stream (id, camera_id, label, url, is_primary) "
           "VALUES (2, 1, 'sub', 'rtsp://192.168.1.10/stream2', 0)");
  exec(db, "INSERT INTO zone (id, camera_id, name, points, zone_type, color) "
           "VALUES (1, 1, 'Door', '[{\"x\":0.1,\"y\":0.2}]', 'alert', "
           "'#00FF00')");
  exec(db, "INSERT INTO zone (id, camera_id, name, points) "
           "VALUES (2, 2, 'Yard', '[]')");
}

CameraMigrationReport migrateSeeded(const CameraFixture& fixture)
{
  {
    const auto source = openFile(fixture.sourcePath);
    const auto schema = applyCameraSchema(
        {.db = source.get(), .schemaPath = ARGUS_CAMERA_SCHEMA_PATH});
    REQUIRE_MESSAGE(schema.ok, schema.error);
    seedSource(source.get());
  }
  return migrateCamera({.sourcePath = fixture.sourcePath,
                        .targetPath = fixture.targetPath,
                        .schemaPath = ARGUS_CAMERA_SCHEMA_PATH});
}

} // namespace

TEST_CASE("migration copies camera tables and verifies them")
{
  const auto fixture = makeFixture();
  const auto report = migrateSeeded(fixture);
  REQUIRE_MESSAGE(report.ok, report.error);
  REQUIRE_FALSE(report.noop);
  REQUIRE(report.tables.size() == 3);

  const std::vector<std::pair<std::string, int64_t>> expectedCounts = {
      std::pair<std::string, int64_t>{"camera", 2},
      std::pair<std::string, int64_t>{"camera_stream", 2},
      std::pair<std::string, int64_t>{"zone", 2},
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
  // FK integrity: the copy keeps camera_id rows pointing at real cameras.
  sqlite3_stmt* violations = nullptr;
  REQUIRE(sqlite3_prepare_v2(target.get(), "PRAGMA foreign_key_check", -1,
                             &violations, nullptr) == SQLITE_OK);
  CHECK(sqlite3_step(violations) == SQLITE_DONE);
  sqlite3_finalize(violations);
}

TEST_CASE("migration of a schema-current target is a verified no-op")
{
  const auto fixture = makeFixture();
  const auto first = migrateSeeded(fixture);
  REQUIRE_MESSAGE(first.ok, first.error);

  const auto second = migrateCamera({.sourcePath = fixture.sourcePath,
                                     .targetPath = fixture.targetPath,
                                     .schemaPath = ARGUS_CAMERA_SCHEMA_PATH});
  REQUIRE_MESSAGE(second.ok, second.error);
  CHECK(second.noop);
  CHECK(second.tables.size() == 3);
  for (const auto& entry : second.tables) {
    CHECK(entry.sourceRows == 0);
    CHECK(entry.sourceChecksum.empty());
    CHECK_FALSE(entry.targetChecksum.empty());
  }

  const auto target = openFile(fixture.targetPath);
  CHECK(countOf(target.get(), "camera") == 2);
  CHECK(countOf(target.get(), "camera_stream") == 2);
  CHECK(countOf(target.get(), "zone") == 2);
}

TEST_CASE("no-op migration does not need the source database")
{
  const auto fixture = makeFixture();
  const auto first = migrateSeeded(fixture);
  REQUIRE_MESSAGE(first.ok, first.error);

  std::filesystem::remove(fixture.sourcePath);
  const auto second = migrateCamera({.sourcePath = fixture.sourcePath,
                                     .targetPath = fixture.targetPath,
                                     .schemaPath = ARGUS_CAMERA_SCHEMA_PATH});
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

TEST_CASE("verification detects a tampered target")
{
  const auto fixture = makeFixture();
  const auto seeded = migrateSeeded(fixture);
  REQUIRE_MESSAGE(seeded.ok, seeded.error);

  const auto target = openUriFile(fixture.targetPath);
  exec(target.get(), "ATTACH DATABASE '" + fixture.sourcePath + "' AS src");
  exec(target.get(), "INSERT INTO zone (camera_id, name, points) "
                     "VALUES (1, 'intruder', '[]')");
  const auto verification = verifyCameraTables({.target = target.get()});
  CHECK_FALSE(verification.ok);
  CHECK_MESSAGE(verification.error.find("zone") != std::string::npos,
                verification.error);
}

TEST_CASE("migration refuses when source and target are the same file")
{
  const auto fixture = makeFixture();
  {
    const auto source = openFile(fixture.sourcePath);
    const auto schema = applyCameraSchema(
        {.db = source.get(), .schemaPath = ARGUS_CAMERA_SCHEMA_PATH});
    REQUIRE_MESSAGE(schema.ok, schema.error);
    seedSource(source.get());
  }

  const auto sizeBefore = std::filesystem::file_size(fixture.sourcePath);
  const auto report = migrateCamera({.sourcePath = fixture.sourcePath,
                                     .targetPath = fixture.sourcePath,
                                     .schemaPath = ARGUS_CAMERA_SCHEMA_PATH});
  CHECK_FALSE(report.ok);
  CHECK_MESSAGE(report.error.find("same file") != std::string::npos,
                report.error);
  CHECK(std::filesystem::file_size(fixture.sourcePath) == sizeBefore);

  const auto source = openFile(fixture.sourcePath);
  CHECK(countOf(source.get(), "camera") == 2);
}

TEST_CASE("migration refuses a source without the camera tables")
{
  const auto fixture = makeFixture();
  {
    const auto source = openFile(fixture.sourcePath);
    exec(source.get(), "CREATE TABLE unrelated (id INTEGER PRIMARY KEY)");
  }

  const auto report = migrateCamera({.sourcePath = fixture.sourcePath,
                                     .targetPath = fixture.targetPath,
                                     .schemaPath = ARGUS_CAMERA_SCHEMA_PATH});
  CHECK_FALSE(report.ok);
  CHECK_MESSAGE(report.error.find("camera table") != std::string::npos,
                report.error);
  CHECK_FALSE(std::filesystem::exists(fixture.targetPath));
}

TEST_CASE("migration fails when the source database is missing")
{
  const auto fixture = makeFixture();
  const auto report = migrateCamera({.sourcePath = fixture.sourcePath,
                                     .targetPath = fixture.targetPath,
                                     .schemaPath = ARGUS_CAMERA_SCHEMA_PATH});
  CHECK_FALSE(report.ok);
  CHECK_MESSAGE(report.error.find("not found") != std::string::npos,
                report.error);
  CHECK_FALSE(std::filesystem::exists(fixture.targetPath));
}
