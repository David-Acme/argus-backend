#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <productivity-migration.hxx>

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

TEST_CASE("productivity schema applies cleanly to an in-memory database")
{
  const auto db = openMemory();
  const auto result = applyProductivitySchema(
      {.db = db.get(), .schemaPath = ARGUS_PRODUCTIVITY_SCHEMA_PATH});
  REQUIRE_MESSAGE(result.ok, result.error);

  const auto tables = queryColumn(db.get(),
      "SELECT name FROM sqlite_master WHERE type = 'table' AND name IN "
      "('project', 'project_task', 'project_member', 'calendar_event', "
      "'calendar_event_share', 'reminder', 'reminder_detail')");
  REQUIRE(tables.size() == 7);

  const auto indexes = queryColumn(db.get(),
      "SELECT name FROM sqlite_master WHERE type = 'index' AND "
      "name LIKE 'idx_%' ORDER BY name");
  CHECK(indexes.size() == 13);
}

TEST_CASE("productivity schema application is idempotent")
{
  const auto db = openMemory();
  const auto first = applyProductivitySchema(
      {.db = db.get(), .schemaPath = ARGUS_PRODUCTIVITY_SCHEMA_PATH});
  REQUIRE_MESSAGE(first.ok, first.error);

  sqlite3_stmt* stmt = nullptr;
  REQUIRE(sqlite3_prepare_v2(db.get(),
                             "INSERT INTO project (id, owner_id, name) "
                             "VALUES (1, 7, 'Home')",
                             -1, &stmt, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_step(stmt) == SQLITE_DONE);
  sqlite3_finalize(stmt);

  const auto second = applyProductivitySchema(
      {.db = db.get(), .schemaPath = ARGUS_PRODUCTIVITY_SCHEMA_PATH});
  REQUIRE_MESSAGE(second.ok, second.error);

  CHECK(queryColumn(db.get(), "SELECT name FROM project WHERE id = 1")
        == std::vector<std::string>{"Home"});
  const auto tables = queryColumn(db.get(),
      "SELECT name FROM sqlite_master WHERE type = 'table' AND name IN "
      "('project', 'project_task', 'project_member', 'calendar_event', "
      "'calendar_event_share', 'reminder', 'reminder_detail')");
  CHECK(tables.size() == 7);
}

TEST_CASE("productivity schema application fails on a missing schema file")
{
  const auto db = openMemory();
  const auto result = applyProductivitySchema(
      {.db = db.get(), .schemaPath = "/argus/no/such/productivity-schema.sql"});
  CHECK_FALSE(result.ok);
  CHECK_MESSAGE(result.error.find("cannot open schema file")
                    != std::string::npos,
                result.error);
}
