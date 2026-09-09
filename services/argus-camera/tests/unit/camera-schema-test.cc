#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <camera-migration.hxx>

#include <memory>
#include <sqlite3.h>

namespace
{

using DbHandle = std::unique_ptr<sqlite3, int (*)(sqlite3*)>;

DbHandle openMemory()
{
  sqlite3* raw = nullptr;
  REQUIRE(sqlite3_open(":memory:", &raw) == SQLITE_OK);
  return DbHandle(raw, sqlite3_close_v2);
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

} // namespace

TEST_CASE("camera schema applies cleanly to an in-memory database")
{
  const auto db = openMemory();
  const auto result = applyCameraSchema(
      {.db = db.get(), .schemaPath = ARGUS_CAMERA_SCHEMA_PATH});
  REQUIRE_MESSAGE(result.ok, result.error);

  const auto tables = queryColumn(db.get(),
      "SELECT name FROM sqlite_master WHERE type = 'table' AND name IN "
      "('camera', 'camera_stream', 'zone')");
  REQUIRE(tables.size() == 3);

  const auto indexes = queryColumn(db.get(),
      "SELECT name FROM sqlite_master WHERE type = 'index' AND "
      "(name LIKE 'idx_camera%' OR name LIKE 'idx_zone%') ORDER BY name");
  CHECK(indexes.size() == 8);
}

TEST_CASE("camera schema application is idempotent")
{
  const auto db = openMemory();
  const auto first = applyCameraSchema(
      {.db = db.get(), .schemaPath = ARGUS_CAMERA_SCHEMA_PATH});
  REQUIRE_MESSAGE(first.ok, first.error);

  sqlite3_stmt* stmt = nullptr;
  REQUIRE(sqlite3_prepare_v2(db.get(),
                             "INSERT INTO camera (id, name, ip) "
                             "VALUES (1, 'Front', '192.168.1.10')",
                             -1, &stmt, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);

  const auto second = applyCameraSchema(
      {.db = db.get(), .schemaPath = ARGUS_CAMERA_SCHEMA_PATH});
  REQUIRE_MESSAGE(second.ok, second.error);

  CHECK(queryColumn(db.get(), "SELECT name FROM camera WHERE id = 1")
        == std::vector<std::string>{"Front"});
  const auto tables = queryColumn(db.get(),
      "SELECT name FROM sqlite_master WHERE type = 'table' AND name IN "
      "('camera', 'camera_stream', 'zone')");
  CHECK(tables.size() == 3);
}

TEST_CASE("camera schema application fails on a missing schema file")
{
  const auto db = openMemory();
  const auto result = applyCameraSchema(
      {.db = db.get(), .schemaPath = "/argus/no/such/camera-schema.sql"});
  CHECK_FALSE(result.ok);
  CHECK_MESSAGE(result.error.find("cannot open schema file")
                    != std::string::npos,
                result.error);
}
