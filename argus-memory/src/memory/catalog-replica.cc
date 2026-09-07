#include "catalog-replica.hxx"

#include <drogon/drogon.h>
#include <shared/contracts/camera-audit-event.hxx>
#include <shared/contracts/sync-operation.hxx>
#include <shared/services/memory/entity-resolver.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <shared/wrapper/nats/nats-subject.hxx>
#include <shared/wrapper/sqlite-stmt/sqlite-stmt.hxx>
#include <trantor/utils/Logger.h>

#include <ctime>
#include <functional>

namespace
{

// Replica upsert/delete replay (Ruling BX). Person and camera tombstone
// (mirroring the source tables' deleted_at predicate); zone and stream rows
// are physical deletes — their gazetteer query has no deleted_at filter.
constexpr const char* UPSERT_PERSON =
    "INSERT INTO catalog_person (id, user_id, name, alias, deleted_at) "
    "VALUES (?, ?, ?, ?, NULL) "
    "ON CONFLICT(id) DO UPDATE SET user_id = excluded.user_id, "
    "name = excluded.name, alias = excluded.alias, deleted_at = NULL";

constexpr const char* TOMBSTONE_PERSON =
    "UPDATE catalog_person SET deleted_at = ? WHERE id = ?";

constexpr const char* UPSERT_CAMERA =
    "INSERT INTO catalog_camera (id, name, deleted_at) VALUES (?, ?, NULL) "
    "ON CONFLICT(id) DO UPDATE SET name = excluded.name, deleted_at = NULL";

constexpr const char* TOMBSTONE_CAMERA =
    "UPDATE catalog_camera SET deleted_at = ? WHERE id = ?";

constexpr const char* UPSERT_ZONE =
    "INSERT INTO catalog_zone (id, name) VALUES (?, ?) "
    "ON CONFLICT(id) DO UPDATE SET name = excluded.name";

constexpr const char* DELETE_ZONE = "DELETE FROM catalog_zone WHERE id = ?";

constexpr const char* UPSERT_STREAM =
    "INSERT INTO catalog_stream (id, label) VALUES (?, ?) "
    "ON CONFLICT(id) DO UPDATE SET label = excluded.label";

constexpr const char* DELETE_STREAM =
    "DELETE FROM catalog_stream WHERE id = ?";

// Snapshot reads run against the SOURCE databases (the legacy table names),
// never against the replicas themselves.
constexpr const char* SNAPSHOT_PERSONS =
    "SELECT id, user_id, name, alias FROM person WHERE deleted_at IS NULL";
constexpr const char* SNAPSHOT_CAMERAS =
    "SELECT id, name FROM camera WHERE deleted_at IS NULL";
constexpr const char* SNAPSHOT_ZONES = "SELECT id, name FROM zone";
constexpr const char* SNAPSHOT_STREAMS =
    "SELECT id, label FROM camera_stream";

bool execStmt(sqlite3* db, const char* sql,
              const std::function<void(SqliteStmt&)>& bind)
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, sql))
    return false;
  if (bind)
    bind(stmt);
  return stmt.step() == SQLITE_DONE;
}

// Per-table emptiness: each replica table that booted empty gets its own
// snapshot fill (an absent identity source must not turn every later boot
// into a camera re-seed).
bool replicaPopulated(sqlite3* db, const char* table)
{
  SqliteStmt probe;
  const std::string count = std::string("SELECT COUNT(*) FROM ") + table;
  if (!probe.prepare(db, count.c_str()))
    return false;
  return probe.step() == SQLITE_ROW && probe.columnInt64(0) > 0;
}

} // namespace

CatalogReplica::CatalogReplica(const Deps& deps)
    : bus_(deps.bus), graph_(deps.graph), resolver_(deps.resolver)
{
}

void CatalogReplica::subscribe()
{
  const auto marshal = [this](const std::function<void(const Json::Value&)>&
                                  apply,
                              std::string payload) {
    drogon::app().getIOLoop(0)->runInLoop([this, apply,
                                           payload = std::move(payload)]() {
      const Json::Value json = json_util::fromString(payload);
      apply(json);
    });
  };

  bus_.subscribe(nats_subject::kIdentityChange,
                 [this, marshal](std::string_view, std::string_view payload) {
                   marshal([this](const Json::Value& json) {
                     applyIdentity(json);
                   }, std::string(payload));
                 });
  bus_.subscribe(nats_subject::kCameraChange,
                 [this, marshal](std::string_view, std::string_view payload) {
                   marshal([this](const Json::Value& json) {
                     applyCamera(json);
                   }, std::string(payload));
                 });
  bus_.subscribe(nats_subject::kSyncChangeWildcard,
                 [this, marshal](std::string_view, std::string_view payload) {
                   marshal([this](const Json::Value& json) {
                     applyStreamRow(json);
                   }, std::string(payload));
                 });
}

void CatalogReplica::applyIdentity(const Json::Value& event)
{
  if (!event.isObject() || event.get("kind", "").asString() != "identity")
    return;
  const std::string table = event.get("table", "").asString();
  if (table != "person")
    return;

  const int64_t id = event.get("id", 0).asInt64();
  if (id <= 0)
    return;
  const bool deleted = event.get("deleted", false).asBool();
  const Json::Value row = event.get("row", Json::Value(Json::objectValue));

  {
    std::scoped_lock lock(graph_.mutex());
    if (deleted) {
      execStmt(graph_.handle(), TOMBSTONE_PERSON,
               [&](SqliteStmt& stmt) {
                 stmt.bindInt64(1, std::time(nullptr));
                 stmt.bindInt64(2, id);
               });
    }
    else {
      execStmt(graph_.handle(), UPSERT_PERSON, [&](SqliteStmt& stmt) {
        stmt.bindInt64(1, id);
        stmt.bindInt64(2, row.get("user_id", 0).asInt64());
        stmt.bindText(3, row.get("name", "").asString());
        stmt.bindText(4, row.get("alias", "").asString());
      });
    }
  }
  resolver_.build();
  LOG_DEBUG << "CatalogReplica: identity " << table << " " << id
            << (deleted ? " tombstoned" : " upserted");
}

void CatalogReplica::applyCamera(const Json::Value& event)
{
  if (!event.isObject())
    return;

  // Audit diff shape (Ruling Y): field-level changes for one row.
  if (event.get("kind", "").asString() == "audit") {
    const auto audit = CameraAuditEvent::fromJson(event);
    if (!audit)
      return;
    const std::string table = tableNameToString(audit->tableName);
    const auto nameIt = audit->changes.find("name");
    const auto labelIt = audit->changes.find("label");
    const auto deletedIt = audit->changes.find("deleted_at");
    {
      std::scoped_lock lock(graph_.mutex());
      if (deletedIt != audit->changes.end()) {
        if (table == "camera")
          execStmt(graph_.handle(), TOMBSTONE_CAMERA, [&](SqliteStmt& stmt) {
            stmt.bindInt64(1, std::time(nullptr));
            stmt.bindInt64(2, audit->recordId);
          });
        else if (table == "zone")
          execStmt(graph_.handle(), DELETE_ZONE,
                   [&](SqliteStmt& stmt) { stmt.bindInt64(1, audit->recordId); });
        else if (table == "camera_stream")
          execStmt(graph_.handle(), DELETE_STREAM,
                   [&](SqliteStmt& stmt) { stmt.bindInt64(1, audit->recordId); });
      }
      else if (table == "camera" && nameIt != audit->changes.end()) {
        execStmt(graph_.handle(), UPSERT_CAMERA, [&](SqliteStmt& stmt) {
          stmt.bindInt64(1, audit->recordId);
          stmt.bindText(2, nameIt->second.current.asString());
        });
      }
      else if (table == "zone" && nameIt != audit->changes.end()) {
        execStmt(graph_.handle(), UPSERT_ZONE, [&](SqliteStmt& stmt) {
          stmt.bindInt64(1, audit->recordId);
          stmt.bindText(2, nameIt->second.current.asString());
        });
      }
      else if (table == "camera_stream" && labelIt != audit->changes.end()) {
        execStmt(graph_.handle(), UPSERT_STREAM, [&](SqliteStmt& stmt) {
          stmt.bindInt64(1, audit->recordId);
          stmt.bindText(2, labelIt->second.current.asString());
        });
      }
      else {
        return;
      }
    }
    resolver_.build();
    return;
  }

  // Plain change-event shape (SocketEmitDto): {operation, option, info}.
  const auto operation = static_cast<SyncOperation>(
      event.get("operation", 0).asInt());
  const std::string option = event.get("option", "").asString();
  if (operation != SyncOperation::Add && operation != SyncOperation::Delete)
    return;
  const Json::Value row = event.get("info", Json::Value(Json::objectValue));
  const int64_t id = row.get("id", 0).asInt64();
  if (id <= 0)
    return;

  {
    std::scoped_lock lock(graph_.mutex());
    if (operation == SyncOperation::Delete) {
      if (option == "camera")
        execStmt(graph_.handle(), TOMBSTONE_CAMERA, [&](SqliteStmt& stmt) {
          stmt.bindInt64(1,
                         row.get("deletedAt", std::time(nullptr)).asInt64());
          stmt.bindInt64(2, id);
        });
      else if (option == "zone")
        execStmt(graph_.handle(), DELETE_ZONE,
                 [&](SqliteStmt& stmt) { stmt.bindInt64(1, id); });
      else if (option == "camera_stream")
        execStmt(graph_.handle(), DELETE_STREAM,
                 [&](SqliteStmt& stmt) { stmt.bindInt64(1, id); });
      else
        return;
    }
    else if (option == "camera") {
      execStmt(graph_.handle(), UPSERT_CAMERA, [&](SqliteStmt& stmt) {
        stmt.bindInt64(1, id);
        stmt.bindText(2, row.get("name", "").asString());
      });
    }
    else if (option == "zone") {
      execStmt(graph_.handle(), UPSERT_ZONE, [&](SqliteStmt& stmt) {
        stmt.bindInt64(1, id);
        stmt.bindText(2, row.get("name", "").asString());
      });
    }
    else if (option == "camera_stream") {
      execStmt(graph_.handle(), UPSERT_STREAM, [&](SqliteStmt& stmt) {
        stmt.bindInt64(1, id);
        stmt.bindText(2, row.get("label", "").asString());
      });
    }
    else {
      return;
    }
  }
  resolver_.build();
}

void CatalogReplica::applyStreamRow(const Json::Value& event)
{
  if (!event.isObject() || event.get("option", "").asString() != "camera_stream")
    return;
  applyCamera(event);
}

void CatalogReplica::seedFromSnapshot(drogon::orm::DbClient* identityDb,
                                      drogon::orm::DbClient* cameraDb)
{
  seedSnapshot({graph_, resolver_, identityDb, cameraDb});
}

void CatalogReplica::seedSnapshot(const SnapshotSources& sources)
{
  if (!sources.identityDb && !sources.cameraDb)
    return;

  int64_t persons = 0;
  int64_t cameras = 0;
  int64_t zones = 0;
  int64_t streams = 0;
  {
    std::scoped_lock lock(sources.graph.mutex());
    sqlite3* db = sources.graph.handle();
    if (!db)
      return;
    const bool personsEmpty = !replicaPopulated(db, "catalog_person");
    const bool camerasEmpty = !replicaPopulated(db, "catalog_camera");
    const bool zonesEmpty = !replicaPopulated(db, "catalog_zone");
    const bool streamsEmpty = !replicaPopulated(db, "catalog_stream");
    if (!personsEmpty && !camerasEmpty && !zonesEmpty && !streamsEmpty) {
      LOG_INFO << "CatalogReplica: replicas already populated; snapshot "
                  "fill skipped";
      return;
    }
    if (sources.identityDb && personsEmpty) {
      for (const auto& row : sources.identityDb->execSqlSync(SNAPSHOT_PERSONS)) {
        execStmt(db, UPSERT_PERSON, [&](SqliteStmt& stmt) {
          stmt.bindInt64(1, row[0].as<int64_t>());
          stmt.bindInt64(2, row[1].isNull() ? 0 : row[1].as<int64_t>());
          stmt.bindText(3, row[2].as<std::string>());
          stmt.bindText(4, row[3].as<std::string>());
        });
        ++persons;
      }
    }
    if (sources.cameraDb && camerasEmpty) {
      for (const auto& row : sources.cameraDb->execSqlSync(SNAPSHOT_CAMERAS)) {
        execStmt(db, UPSERT_CAMERA, [&](SqliteStmt& stmt) {
          stmt.bindInt64(1, row[0].as<int64_t>());
          stmt.bindText(2, row[1].as<std::string>());
        });
        ++cameras;
      }
    }
    if (sources.cameraDb && zonesEmpty) {
      for (const auto& row : sources.cameraDb->execSqlSync(SNAPSHOT_ZONES)) {
        execStmt(db, UPSERT_ZONE, [&](SqliteStmt& stmt) {
          stmt.bindInt64(1, row[0].as<int64_t>());
          stmt.bindText(2, row[1].as<std::string>());
        });
        ++zones;
      }
    }
    if (sources.cameraDb && streamsEmpty) {
      for (const auto& row : sources.cameraDb->execSqlSync(SNAPSHOT_STREAMS)) {
        execStmt(db, UPSERT_STREAM, [&](SqliteStmt& stmt) {
          stmt.bindInt64(1, row[0].as<int64_t>());
          stmt.bindText(2, row[1].as<std::string>());
        });
        ++streams;
      }
    }
  }
  sources.resolver.build();
  LOG_INFO << "CatalogReplica: snapshot filled persons=" << persons
           << " cameras=" << cameras << " zones=" << zones
           << " streams=" << streams;
}
