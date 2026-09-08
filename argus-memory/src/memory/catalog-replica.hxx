#pragma once

#include <drogon/orm/DbClient.h>
#include <json/value.h>
#include <memory>
#include <shared/services/memory/sqlite-graph.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>
#include <sqlite3.h>
#include <string>

class EntityResolver;

// Catalog replica feed over the identity and camera change subjects.
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

  // Subscribes the change feed: identity + camera subjects and the sync wildcard.
  void subscribe();

  // One snapshot fill per replica table that booted empty.
  void seedFromSnapshot(drogon::orm::DbClient* identityDb,
                        drogon::orm::DbClient* cameraDb);

  // Boot fill shared with the no-NATS path.
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
