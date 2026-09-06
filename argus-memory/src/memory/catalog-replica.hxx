#pragma once

#include <drogon/orm/DbClient.h>
#include <json/value.h>
#include <memory>
#include <shared/services/memory/sqlite-graph.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>
#include <sqlite3.h>
#include <string>

class EntityResolver;

// Catalog replica feed (Ruling BX): replays the identity and camera change
// subjects into the local replica tables, subscribes the sync wildcard for
// camera_stream rows the camera subject never carries, and re-runs the
// gazetteer build after every applied change. On boot, tables still empty
// get one snapshot fill from the read-only source clients.
class CatalogReplica
{
public:
  struct Deps
  {
    NatsBus& bus;
    SqliteGraph& graph;
    EntityResolver& resolver;
  };

  explicit CatalogReplica(const Deps& deps);

  // The change feed: identity + camera subjects, plus the sync wildcard
  // filtered to camera_stream rows. Handlers marshal onto the Drogon loop
  // (cnats dispatcher threads must not block).
  void subscribe();

  // One snapshot fill per replica table that booted empty; either source
  // client may be absent (null skips its tables).
  void seedFromSnapshot(drogon::orm::DbClient* identityDb,
                        drogon::orm::DbClient* cameraDb);

  // Boot fill shared with the no-NATS path (a fresh boot without a change
  // feed still fills the replicas from the read-only sources).
  struct SnapshotSources
  {
    SqliteGraph& graph;
    EntityResolver& resolver;
    drogon::orm::DbClient* identityDb;
    drogon::orm::DbClient* cameraDb;
  };
  static void seedSnapshot(const SnapshotSources& sources);

  void applyIdentity(const Json::Value& event);
  void applyCamera(const Json::Value& event);
  void applyStreamRow(const Json::Value& event);

private:
  NatsBus& bus_;
  SqliteGraph& graph_;
  EntityResolver& resolver_;
};
