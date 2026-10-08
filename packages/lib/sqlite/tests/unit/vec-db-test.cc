#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <sqlite/sqlite-stmt.hxx>
#include <sqlite/vec-db.hxx>

#include <filesystem>
#include <sqlite3.h>
#include <string>
#include <string_view>
#include <unistd.h>

#define SQLITE_CORE
#include "sqlite-vec.h"

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

bool vecInit(sqlite3* db)
{
  char* error = nullptr;
  const int rc = sqlite3_vec_init(db, &error, nullptr);
  if (error != nullptr)
    sqlite3_free(error);
  return rc == SQLITE_OK;
}

int denyRowidsShadowDrop(void*, int action, const char* name, const char*, const char*,
                         const char*)
{
  if (action == SQLITE_DROP_TABLE && name != nullptr && std::string_view(name).ends_with("_rowids"))
    return SQLITE_DENY;
  return SQLITE_OK;
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

TEST_CASE("a vec0 destroy that fails runs the teardown twice without a double free")
{
  const auto file = scratch("destroy");
  std::filesystem::remove(file);

  sqlite3* db = nullptr;
  REQUIRE(sqlite3_open(file.string().c_str(), &db) == SQLITE_OK);
  REQUIRE(vecInit(db));
  REQUIRE(sqlite3_exec(db, "CREATE VIRTUAL TABLE teardown_probe USING vec0(emb float[4], label text)",
                       nullptr, nullptr, nullptr) == SQLITE_OK);

  sqlite3_set_authorizer(db, denyRowidsShadowDrop, nullptr);
  CHECK(sqlite3_exec(db, "DROP TABLE teardown_probe", nullptr, nullptr, nullptr) == SQLITE_ERROR);
  sqlite3_set_authorizer(db, nullptr, nullptr);

  CHECK(hasTable(db, "teardown_probe"));
  CHECK(sqlite3_close(db) == SQLITE_OK);
  std::filesystem::remove(file);
}
