#pragma once

#include <cstdint>
#include <json/value.h>
#include <shared/services/memory/sqlite-graph.hxx>
#include <nats/nats-bus.hxx>
#include <string>
#include <vector>

class EntityResolver;

class CatalogReplica
{
public:
  struct PersonRow
  {
    int64_t id{0};
    int64_t userId{0};
    std::string name;
    std::string alias;
  };

  struct CameraRow
  {
    int64_t id{0};
    std::string name;
  };

  struct ZoneRow
  {
    int64_t id{0};
    std::string name;
  };

  struct StreamRow
  {
    int64_t id{0};
    std::string label;
  };

  struct Snapshot
  {
    std::vector<PersonRow> persons;
    std::vector<CameraRow> cameras;
    std::vector<ZoneRow> zones;
    std::vector<StreamRow> streams;
  };

  struct Deps
  {
    NatsBus& bus;
    SqliteGraph& graph;
    EntityResolver& resolver;
  };

  explicit CatalogReplica(const Deps& deps);

  void subscribe();

  void seedFromSnapshot(const Snapshot& snapshot);

  struct SnapshotSources
  {
    SqliteGraph& graph;
    EntityResolver& resolver;
    const Snapshot& snapshot;
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
