#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <memory/catalog-replica.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/memory/entity-resolver.hxx>
#include <shared/services/memory/sqlite-graph.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>
#include <shared/wrapper/sqlite-stmt/sqlite-stmt.hxx>

#include <json/json.h>
#include <sqlite3.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace
{

#ifndef ARGUS_TEST_MEMORY_SCHEMA
#define ARGUS_TEST_MEMORY_SCHEMA "database/memory-schema.sql"
#endif

constexpr const char* kScratchConfig = "memory-replica-test.toml";
constexpr const char* kScratchDir = "/tmp/f46-memory-replica";

// Drogon's sqlite clients reassert SQLITE_CONFIG_MULTITHREAD: it must be
// configured before any connection exists (Ruling BW), so pin it at static
// init, before main runs anything.
const int sqliteThreadMode =
    sqlite3_config(SQLITE_CONFIG_MULTITHREAD);

Json::Value eventJson(const std::string& raw)
{
  Json::Value json;
  Json::Reader reader;
  REQUIRE(reader.parse(raw, json));
  return json;
}

int64_t countRows(sqlite3* db, const char* sql)
{
  SqliteStmt stmt;
  REQUIRE(stmt.prepare(db, sql));
  REQUIRE(stmt.step() == SQLITE_ROW);
  return stmt.columnInt64(0);
}

bool rowExists(sqlite3* db, const char* sql, int64_t id)
{
  SqliteStmt stmt;
  REQUIRE(stmt.prepare(db, sql));
  stmt.bindInt64(1, id);
  return stmt.step() == SQLITE_ROW;
}

drogon::orm::DbClientPtr
openScratchSource(const std::string& path,
                  const std::vector<const char*>& statements)
{
  std::filesystem::remove(path);
  auto client = drogon::orm::DbClient::newSqlite3Client("filename=" + path, 1);
  for (const char* statement : statements)
    client->execSqlSync(statement);
  return client;
}

} // namespace

TEST_CASE("catalog replicas replay identity and camera events and rebuild "
          "the gazetteer")
{
  std::remove(kScratchConfig);
  {
    std::ofstream config(kScratchConfig);
    config << "[database]\nfile = \"" << kScratchDir << "/memory.db\"\n"
           << "[memory]\n"
           << "schema_file = \"" << ARGUS_TEST_MEMORY_SCHEMA << "\"\n"
           << "catalog_person_table = \"catalog_person\"\n"
           << "catalog_camera_table = \"catalog_camera\"\n"
           << "catalog_zone_table = \"catalog_zone\"\n"
           << "catalog_stream_table = \"catalog_stream\"\n"
           << "create_face_vec = false\n";
  }
  ConfigService::load(kScratchConfig);
  std::filesystem::create_directories(kScratchDir);
  std::filesystem::remove(std::string(kScratchDir) + "/memory.db");

  NatsBus bus;
  SqliteGraph graph;
  REQUIRE(graph.open(std::string(kScratchDir) + "/memory.db"));
  graph.applySchema();
  EntityResolver resolver(graph);

  CatalogReplica replica({.bus = bus, .graph = graph, .resolver = resolver});
  sqlite3* db = graph.handle();
  REQUIRE(db != nullptr);

  // ── identity change upserts the person replica and rebuilds the
  // gazetteer (Ruling BX) ──────────────────────────────────────────────
  replica.applyIdentity(eventJson(
      R"({"kind":"identity","table":"person","id":7,"deleted":false,
          "row":{"id":7,"user_id":42,"name":"Ana Garcia","alias":""}})"));
  CHECK(countRows(db, "SELECT COUNT(*) FROM catalog_person") == 1);
  resolver.build();
  const auto hits = resolver.resolve("Ana Garcia");
  REQUIRE_FALSE(hits.empty());
  bool personHit = false;
  for (const auto& hit : hits)
    if (hit.catalog == "person" && hit.catalogId == 7)
      personHit = true;
  CHECK(personHit);

  // ── an identity tombstone hides the person from the gazetteer ─────────
  replica.applyIdentity(eventJson(
      R"({"kind":"identity","table":"person","id":7,"deleted":true,
          "row":{}})"));
  CHECK(rowExists(db,
                  "SELECT 1 FROM catalog_person WHERE id = ? AND "
                  "deleted_at IS NULL",
                  7) == false);
  resolver.build();
  CHECK(resolver.resolve("Ana Garcia").empty());

  // ── camera audit diffs replay into camera/zone/stream replicas ────────
  replica.applyCamera(eventJson(
      R"({"kind":"audit","record_id":3,"table_name":"camera",
          "changes":{"name":{"previous":"cam vieja","current":"cam nueva"}},
          "priority":0,"create_user_id":1,
          "event_timestamp":1770000000})"));
  CHECK(rowExists(db, "SELECT 1 FROM catalog_camera WHERE id = ?", 3));
  resolver.build();
  CHECK_FALSE(resolver.resolve("cam nueva").empty());

  replica.applyCamera(eventJson(
      R"({"kind":"audit","record_id":5,"table_name":"zone",
          "changes":{"name":{"previous":"salon","current":"estar"}},
          "priority":0,"create_user_id":1,
          "event_timestamp":1770000000})"));
  CHECK(rowExists(db, "SELECT 1 FROM catalog_zone WHERE id = ?", 5));

  replica.applyCamera(eventJson(
      R"({"kind":"audit","record_id":9,"table_name":"camera_stream",
          "changes":{"label":{"previous":"","current":"entrada"}},
          "priority":0,"create_user_id":1,
          "event_timestamp":1770000000})"));
  CHECK(rowExists(db, "SELECT 1 FROM catalog_stream WHERE id = ?", 9));

  // ── camera tombstone: the replica mirrors the source deleted_at
  // predicate; zone/stream rows are removed physically ───────────────────
  replica.applyCamera(eventJson(
      R"({"kind":"audit","record_id":3,"table_name":"camera",
          "changes":{"deleted_at":{"previous":null,"current":1770000000}},
          "priority":0,"create_user_id":1,
          "event_timestamp":1770000000})"));
  CHECK(rowExists(db,
                  "SELECT 1 FROM catalog_camera WHERE id = ? AND "
                  "deleted_at IS NULL",
                  3) == false);

  replica.applyCamera(eventJson(
      R"({"kind":"audit","record_id":5,"table_name":"zone",
          "changes":{"deleted_at":{"previous":null,"current":1770000000}},
          "priority":0,"create_user_id":1,
          "event_timestamp":1770000000})"));
  CHECK(rowExists(db, "SELECT 1 FROM catalog_zone WHERE id = ?", 5) == false);

  // ── sync-wildcard rows: camera_stream rides argus.*.v1.change (Ruling
  // BX); non-stream options are ignored ─────────────────────────────────
  replica.applyStreamRow(eventJson(
      R"({"operation":4,"option":"camera_stream",
          "info":{"id":11,"label":"patio"}})"));
  CHECK(rowExists(db, "SELECT 1 FROM catalog_stream WHERE id = ?", 11));

  replica.applyStreamRow(eventJson(
      R"({"operation":4,"option":"camera",
          "info":{"id":12,"name":"garaje"}})"));
  CHECK(rowExists(db, "SELECT 1 FROM catalog_camera WHERE id = ?", 12) ==
        false);

  // ── a sync delete removes the stream row ──────────────────────────────
  replica.applyStreamRow(eventJson(
      R"({"operation":5,"option":"camera_stream",
          "info":{"id":11,"label":"patio"}})"));
  CHECK(rowExists(db, "SELECT 1 FROM catalog_stream WHERE id = ?", 11) ==
        false);

  graph.close();
  std::remove(kScratchConfig);
}

TEST_CASE("a boot with empty replica tables takes one snapshot fill from the "
          "read-only sources and never re-seeds")
{
  std::remove(kScratchConfig);
  {
    std::ofstream config(kScratchConfig);
    config << "[database]\nfile = \"" << kScratchDir << "/snapshot.db\"\n"
           << "[memory]\n"
           << "schema_file = \"" << ARGUS_TEST_MEMORY_SCHEMA << "\"\n"
           << "catalog_person_table = \"catalog_person\"\n"
           << "catalog_camera_table = \"catalog_camera\"\n"
           << "catalog_zone_table = \"catalog_zone\"\n"
           << "catalog_stream_table = \"catalog_stream\"\n"
           << "create_face_vec = false\n";
  }
  ConfigService::load(kScratchConfig);
  std::filesystem::create_directories(kScratchDir);
  std::filesystem::remove(std::string(kScratchDir) + "/snapshot.db");

  const auto identityDb = openScratchSource(
      std::string(kScratchDir) + "/identity.db",
      {"CREATE TABLE person (id INTEGER PRIMARY KEY, user_id INTEGER, "
       "name TEXT, alias TEXT, deleted_at INTEGER)",
       "INSERT INTO person (id, user_id, name, alias, deleted_at) VALUES "
       "(7, 42, 'Ana Garcia', '', NULL), (8, 43, 'Beto Ruiz', 'betito', "
       "NULL)"});
  const auto cameraDb = openScratchSource(
      std::string(kScratchDir) + "/camera.db",
      {"CREATE TABLE camera (id INTEGER PRIMARY KEY, name TEXT, "
       "deleted_at INTEGER)",
       "CREATE TABLE zone (id INTEGER PRIMARY KEY, name TEXT)",
       "CREATE TABLE camera_stream (id INTEGER PRIMARY KEY, label TEXT)",
       "INSERT INTO camera (id, name, deleted_at) VALUES (3, 'cam nueva', "
       "NULL), (4, 'cam muerta', 1770000000)",
       "INSERT INTO zone (id, name) VALUES (5, 'estar')",
       "INSERT INTO camera_stream (id, label) VALUES (9, 'entrada')"});

  NatsBus bus;
  SqliteGraph graph;
  REQUIRE(graph.open(std::string(kScratchDir) + "/snapshot.db"));
  graph.applySchema();
  EntityResolver resolver(graph);
  CatalogReplica replica({.bus = bus, .graph = graph, .resolver = resolver});
  sqlite3* db = graph.handle();
  REQUIRE(db != nullptr);

  replica.seedFromSnapshot(identityDb.get(), cameraDb.get());
  CHECK(countRows(db, "SELECT COUNT(*) FROM catalog_person") == 2);
  CHECK(countRows(db, "SELECT COUNT(*) FROM catalog_camera") == 1);
  CHECK(rowExists(db, "SELECT 1 FROM catalog_camera WHERE id = ?", 4) ==
        false);
  CHECK(countRows(db, "SELECT COUNT(*) FROM catalog_zone") == 1);
  CHECK(countRows(db, "SELECT COUNT(*) FROM catalog_stream") == 1);
  resolver.build();
  CHECK_FALSE(resolver.resolve("Beto Ruiz").empty());
  CHECK_FALSE(resolver.resolve("cam nueva").empty());

  // A populated table set is never re-seeded: the second call is a no-op.
  replica.seedFromSnapshot(identityDb.get(), cameraDb.get());
  CHECK(countRows(db, "SELECT COUNT(*) FROM catalog_person") == 2);

  // Absent source clients skip their tables without touching the others.
  std::filesystem::remove(std::string(kScratchDir) + "/empty.db");
  SqliteGraph emptyGraph;
  REQUIRE(emptyGraph.open(std::string(kScratchDir) + "/empty.db"));
  emptyGraph.applySchema();
  EntityResolver emptyResolver(emptyGraph);
  CatalogReplica emptyReplica(
      {.bus = bus, .graph = emptyGraph, .resolver = emptyResolver});
  emptyReplica.seedFromSnapshot(nullptr, cameraDb.get());
  sqlite3* emptyDb = emptyGraph.handle();
  CHECK(countRows(emptyDb, "SELECT COUNT(*) FROM catalog_person") == 0);
  CHECK(countRows(emptyDb, "SELECT COUNT(*) FROM catalog_camera") == 1);

  // The no-NATS boot path fills through the static entry (main.cc calls it
  // when the change feed never connected).
  std::filesystem::remove(std::string(kScratchDir) + "/busless.db");
  SqliteGraph buslessGraph;
  REQUIRE(buslessGraph.open(std::string(kScratchDir) + "/busless.db"));
  buslessGraph.applySchema();
  EntityResolver buslessResolver(buslessGraph);
  CatalogReplica::seedSnapshot({buslessGraph, buslessResolver,
                                identityDb.get(), cameraDb.get()});
  sqlite3* buslessDb = buslessGraph.handle();
  CHECK(countRows(buslessDb, "SELECT COUNT(*) FROM catalog_person") == 2);
  CHECK(countRows(buslessDb, "SELECT COUNT(*) FROM catalog_zone") == 1);
  buslessResolver.build();
  CHECK_FALSE(buslessResolver.resolve("Ana Garcia").empty());

  graph.close();
  emptyGraph.close();
  buslessGraph.close();

  graph.close();
  emptyGraph.close();
  std::remove(kScratchConfig);
}
