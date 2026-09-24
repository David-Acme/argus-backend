#pragma once

#include <cstdint>
#include <json/value.h>
#include <nats/nats-bus.hxx>
#include <optional>
#include <shared/services/memory/sqlite-graph.hxx>
#include <string>
#include <vector>

class EntityResolver;

namespace catalog_feed
{
struct Feed
{
  std::string stream;
  std::string subject;
  std::string durable;
};

const std::vector<Feed>& defaults();
}

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
  ~CatalogReplica();

  void subscribe();
  void stop();

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

private:
  struct Attachment
  {
    catalog_feed::Feed feed;
    std::optional<uint64_t> subscription;
  };

  struct ApplyInput
  {
    std::string subject;
    std::string payload;
    NatsBus::DurableSettlement settlement;
  };

  void applyPayload(const ApplyInput& input);
  bool trySubscribe(Attachment& attachment);
  bool subscribePending();
  void scheduleSubscribeRetry();

  NatsBus& bus_;
  SqliteGraph& graph_;
  EntityResolver& resolver_;
  std::vector<Attachment> attachments_;
  std::optional<uint64_t> retryTimer_;
};
