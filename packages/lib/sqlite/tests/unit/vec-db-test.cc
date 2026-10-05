#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <sqlite/sqlite-stmt.hxx>
#include <sqlite/vec-db.hxx>

#include <filesystem>
#include <sqlite3.h>
#include <string>
#include <unistd.h>

namespace
{

std::filesystem::path scratch(const std::string& name)
{
  return std::filesystem::temp_directory_path() /
         ("vec-db-test-" + std::to_string(::getpid()) + "-" + name + ".db");
}

bool hasTable(sqlite3* db, const std::string& name)
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, "SELECT count(*) FROM sqlite_master WHERE type = 'table' AND name = ?"))
    return false;
  stmt.bindText(1, name);
  return stmt.step() == SQLITE_ROW && stmt.columnInt64(0) == 1;
}

}

TEST_CASE("pointing the vec connection at another file reopens it on that file")
{
  const auto first = scratch("first");
  const auto second = scratch("second");
  std::filesystem::remove(first);
  std::filesystem::remove(second);
  {
    VecDb vec;
    vec.setDbFile(first.string());
    sqlite3* db = vec.handle();
    REQUIRE(db != nullptr);
    REQUIRE(sqlite3_exec(db, "CREATE TABLE marker (id INTEGER)", nullptr, nullptr, nullptr) ==
            SQLITE_OK);
    CHECK(hasTable(vec.handle(), "marker"));

    vec.setDbFile(first.string());
    CHECK(vec.handle() == db);

    vec.setDbFile(second.string());
    REQUIRE(vec.handle() != nullptr);
    CHECK_FALSE(hasTable(vec.handle(), "marker"));
    CHECK(hasTable(vec.handle(), "memory_vec"));

    vec.setDbFile(first.string());
    CHECK(hasTable(vec.handle(), "marker"));
  }
  std::filesystem::remove(first);
  std::filesystem::remove(second);
}
