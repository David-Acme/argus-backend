#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <shared/repositories/memory-graph/memory-graph-repository.hxx>
#include <shared/services/memory/sqlite-graph.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/wrapper/sqlite-stmt/sqlite-stmt.hxx>

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sqlite3.h>
#include <string>
#include <unistd.h>

#ifndef ARGUS_TEST_MEMORY_SCHEMA
#error "ARGUS_TEST_MEMORY_SCHEMA must point at database/schema.sql"
#endif

namespace
{
int nameCounter()
{
  static std::atomic<int> counter{0};
  return counter.fetch_add(1);
}

std::string uniqueStem(const std::string& prefix)
{
  return prefix + "-" + std::to_string(::getpid()) + "-" +
         std::to_string(nameCounter());
}

class TempDir
{
public:
  explicit TempDir(const std::string& stem)
      : path_(std::filesystem::temp_directory_path() / stem)
  {
    std::filesystem::remove_all(path_);
    std::filesystem::create_directories(path_);
  }

  ~TempDir() { std::filesystem::remove_all(path_); }

  std::string file(const std::string& name) const
  {
    return (path_ / name).string();
  }

private:
  std::filesystem::path path_;
};

int64_t scalarInt(sqlite3* db, const std::string& sql)
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, sql.c_str()))
    return -1;
  if (stmt.step() != SQLITE_ROW)
    return -1;
  return stmt.columnInt64(0);
}

std::string scalarText(sqlite3* db, const std::string& sql)
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, sql.c_str()))
    return {};
  if (stmt.step() != SQLITE_ROW)
    return {};
  return stmt.columnText(0);
}

void writeConfig(const std::string& path, const std::string& dbPath)
{
  std::ofstream out(path, std::ios::trunc);
  out << "[database]\nfile = \"" << dbPath << "\"\n"
      << "[memory]\nschema_file = \"" << ARGUS_TEST_MEMORY_SCHEMA << "\"\n"
      << "create_face_vec = false\n";
}
} // namespace

TEST_CASE("the encounter inbox is durable, exact and fail-closed")
{
  const TempDir dir(uniqueStem("encounter-inbox-test"));
  const std::string configPath = dir.file("config.toml");
  const std::string dbPath = dir.file("memory.db");
  writeConfig(configPath, dbPath);
  ConfigService::load(configPath);

  SqliteGraph graph;
  REQUIRE(graph.open(dbPath));
  graph.applySchema();
  sqlite3* db = graph.handle();
  REQUIRE(db != nullptr);
  CHECK(scalarInt(db, "SELECT COUNT(*) FROM sqlite_master WHERE type = "
                      "'table' AND name = 'encounter_closed_inbox'") == 1);
  CHECK(scalarText(db, "SELECT sql FROM sqlite_master WHERE type = 'table' "
                       "AND name = 'encounter_closed_inbox'")
            .find("dead_lettered") != std::string::npos);

  MemoryGraphRepository repository;
  const EncounterClosedReceiptInput first{.eventId = "enc:1",
                                          .fingerprint = "fp-1",
                                          .at = 100};
  CHECK_FALSE(repository.claimEncounterClosed(db, first).duplicate);
  CHECK_FALSE(repository.claimEncounterClosed(db, first).duplicate);
  CHECK(repository.markEncounterDispatched(
      db, {.eventId = "enc:1", .at = 101}));
  CHECK(repository.claimEncounterClosed(db, first).duplicate);
  CHECK(scalarText(db, "SELECT status FROM encounter_closed_inbox WHERE "
                       "event_id = 'enc:1'") == "dispatched");

  const EncounterClosedReceiptInput conflict{.eventId = "enc:2",
                                             .fingerprint = "fp-2a",
                                             .at = 102};
  CHECK_FALSE(repository.claimEncounterClosed(db, conflict).duplicate);
  CHECK(repository.claimEncounterClosed(
      db, {.eventId = "enc:2", .fingerprint = "fp-2b", .at = 103}).duplicate);
  CHECK(scalarText(db, "SELECT status FROM encounter_closed_inbox WHERE "
                       "event_id = 'enc:2'") == "conflict");

  const EncounterClosedReceiptInput poison{.eventId = "enc:3",
                                           .fingerprint = "fp-3",
                                           .at = 104};
  CHECK_FALSE(repository.claimEncounterClosed(db, poison).duplicate);
  CHECK(repository.noteEncounterAttempt(
      db, {.eventId = "enc:3", .at = 105}) == 1);
  CHECK(repository.noteEncounterAttempt(
      db, {.eventId = "enc:3", .at = 106}) == 2);
  CHECK(repository.markEncounterDeadLettered(
      db, {.eventId = "enc:3", .at = 107}));
  CHECK(repository.claimEncounterClosed(db, poison).duplicate);
  CHECK(scalarText(db, "SELECT status FROM encounter_closed_inbox WHERE "
                       "event_id = 'enc:3'") == "dead_lettered");

  REQUIRE(sqlite3_exec(db, "DROP TABLE encounter_closed_inbox", nullptr,
                       nullptr, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_exec(db,
                       "CREATE TABLE encounter_closed_inbox (event_id TEXT "
                       "NOT NULL PRIMARY KEY, fingerprint TEXT NOT NULL "
                       "DEFAULT '', attempts INTEGER NOT NULL DEFAULT 0, "
                       "status TEXT NOT NULL DEFAULT 'received', created_at "
                       "INTEGER NOT NULL DEFAULT 0, updated_at INTEGER NOT "
                       "NULL DEFAULT 0)",
                       nullptr, nullptr, nullptr) == SQLITE_OK);
  REQUIRE(sqlite3_exec(db,
                       "INSERT INTO encounter_closed_inbox (event_id, status) "
                       "VALUES ('enc:4', 'bogus')",
                       nullptr, nullptr, nullptr) == SQLITE_OK);
  CHECK(repository.claimEncounterClosed(db, {.eventId = "enc:4",
                                             .fingerprint = "fp-4",
                                             .at = 108}).duplicate);
  CHECK(scalarText(db, "SELECT status FROM encounter_closed_inbox WHERE "
                       "event_id = 'enc:4'") == "dead_lettered");

  graph.close();
}

TEST_CASE("opening an existing store gains the encounter inbox")
{
  const TempDir dir(uniqueStem("encounter-inbox-migrate-test"));
  const std::string configPath = dir.file("config.toml");
  const std::string dbPath = dir.file("memory.db");
  writeConfig(configPath, dbPath);
  ConfigService::load(configPath);

  {
    SqliteGraph legacy;
    REQUIRE(legacy.open(dbPath));
    legacy.applySchema();
    REQUIRE(sqlite3_exec(legacy.handle(), "DROP TABLE encounter_closed_inbox",
                         nullptr, nullptr, nullptr) == SQLITE_OK);
    REQUIRE(sqlite3_exec(legacy.handle(),
                         "INSERT INTO memory_entity (kind, canonical, lang, "
                         "created_at, updated_at, person_id) VALUES "
                         "('person', 'ada', 'es', 1, 1, NULL)",
                         nullptr, nullptr, nullptr) == SQLITE_OK);
    legacy.close();
  }

  SqliteGraph reopened;
  REQUIRE(reopened.open(dbPath));
  sqlite3* db = reopened.handle();
  REQUIRE(db != nullptr);
  CHECK(scalarInt(db, "SELECT COUNT(*) FROM sqlite_master WHERE type = "
                      "'table' AND name = 'encounter_closed_inbox'") == 1);
  CHECK(scalarInt(db, "SELECT COUNT(*) FROM memory_entity WHERE canonical = "
                      "'ada'") == 1);
  reopened.close();
}
