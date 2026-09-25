#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

struct EntityCreateInput
{
  std::string kind;
  std::string canonical;
  std::string lang;
  std::optional<int64_t> personId;
};

struct AliasResolveInput
{
  std::string surface;
  std::string norm;
  std::string lang;
};

struct AliasCreateInput
{
  int64_t entityId;
  std::string surface;
  std::string norm;
  std::string lang;
  std::string personFrame;
  float confidence;
};

struct AliasInfo
{
  std::string surface;
  std::string personFrame;
};

struct FactUpsertInput
{
  int64_t entityId;
  std::string predicate;
  std::string value;
  std::string canonical;
  std::string type;
  int priority;
  float confidence;
  std::string lang;
  std::string scope;
  int64_t refId;
  int64_t now;
  std::optional<int64_t> sourceId;
};

struct RecallEntityInput
{
  int64_t entityId;
  std::string scope;
  int64_t refId;
  int maxHops;
  int limit;
};

struct RecallHit
{
  int64_t factId;
  int64_t entityId;
  std::string predicate;
  std::string value;
  std::string canonical;
  std::string type;
  int priority;
  float confidence;
  int64_t hitCount;
  int hops;
  float score;
};

struct EpisodesBetweenInput
{
  std::string scope;
  int64_t refId;
  int64_t from;
  int64_t to;
  int limit;
};

struct SourceCreateInput
{
  std::string channel;
  std::string turnRef;
  int64_t at;
};

struct ProcedureRecordInput
{
  std::string name;
  std::string goal;
  std::string steps;
};

struct EpisodeCreateInput
{
  std::string kind;
  std::string summary;
  std::string actor;
  int64_t occurredAt;
  std::string sessionId;
  std::string lang;
  std::string scope;
  int64_t refId;
  float salience;
  std::optional<int64_t> sourceId;
  std::vector<int64_t> mentionEntityIds;
};

class SemanticGraph
{
public:
  virtual ~SemanticGraph() = default;

  virtual int64_t createEntity(const EntityCreateInput& input) = 0;
  virtual std::optional<int64_t>
  resolveEntity(const AliasResolveInput& input) = 0;
  virtual int64_t addAlias(const AliasCreateInput& input) = 0;
  virtual std::vector<AliasInfo> aliasesForEntity(int64_t entityId) = 0;

  virtual int64_t upsertFact(const FactUpsertInput& input) = 0;
  virtual bool closeFact(int64_t factId, int64_t at) = 0;
  virtual std::vector<RecallHit>
  factsForEntity(const RecallEntityInput& input) = 0;

  virtual int64_t recordEpisode(const EpisodeCreateInput& input) = 0;
  virtual std::vector<int64_t>
  episodesBetween(const EpisodesBetweenInput& input) = 0;
  virtual int64_t createSource(const SourceCreateInput& input) = 0;
};
