#pragma once

#include <cstdint>
#include <optional>
#include <shared/repositories/memory-graph/memory-graph-repository.hxx>
#include <shared/services/embedding/embedding-service.hxx>
#include <shared/services/memory/entity-resolver.hxx>
#include <shared/services/sqlite/vec-db.hxx>
#include <string>
#include <vector>

class SqliteGraph;

struct GraphRecallInput
{
  std::string text;
  std::string lang;
  std::string scope;
  int64_t refId;
  int maxHops = 1;
  int limit = 8;
  int64_t addresseeEntityId = 0;
};

struct GraphRecallHit
{
  int64_t factId;
  int64_t entityId;
  std::string rendered;
  std::string predicate;
  std::string value;
  std::string canonical;
  std::string type;
  int priority;
  float confidence;
  int hops;
  float score;
};

struct GraphRecallResult
{
  std::vector<GraphRecallHit> hits;
  std::vector<int64_t> usedIds;
  std::string block;
  std::vector<int64_t> resolvedEntityIds;
  bool entityAnchored = false;
};

struct RecallInput
{
  int64_t userId;
  std::string text;
  std::string lang;
  std::vector<int64_t> personIds;
};

struct RecalledMemory
{
  int64_t id;
  std::string content;
  float score;
};

struct RecallContext
{
  std::string prependText;
  std::string profileText;
  std::vector<int64_t> usedIds;
};

class GraphRecall
{
public:
  GraphRecall(SqliteGraph& graph, EntityResolver& resolver,
              EmbeddingService& embedding, VecDb& vecDb)
      : graph_(graph), resolver_(resolver), embedding_(embedding), vecDb_(vecDb)
  {
  }

  GraphRecallResult recall(const GraphRecallInput& input);

  std::string render(int64_t entityId, int64_t addresseeEntityId,
                     const std::string& canonical) const;

private:
  struct Tuning
  {
    float margin = 0.05F;
    float floorSim = 0.80F;
    float strictSim = 0.86F;
    int maxFacts = 2;
    int minHits = 4;
  };

  const Tuning& tuning() const;

  void collectSemantic(const GraphRecallInput& input,
                       GraphRecallResult& result);

  SqliteGraph& graph_;
  EntityResolver& resolver_;
  EmbeddingService& embedding_;
  VecDb& vecDb_;
  MemoryGraphRepository repo_;
  mutable std::optional<Tuning> tuning_;
};
