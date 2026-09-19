#pragma once

#include <cstdint>
#include <optional>
#include <shared/contracts/tool-contracts.hxx>
#include <shared/services/extract/extract-contracts.hxx>
#include <shared/services/memory/entity-resolver.hxx>
#include <shared/services/memory/rule-parser.hxx>
#include <string>
#include <vector>

class SqliteGraph;

struct Observation
{
  std::string channel;
  std::string text;
  std::string actor;
  int64_t at;
  int64_t userId;
  std::string lang;
  std::string sessionId;
  std::vector<int64_t> entitiesHint;
  bool allowModel = true;
  bool salient = false;
  // Upstream already decided this is a save; no trigger or extraction needed.
  bool decided = false;
  // Forces the stored fact type; the reminder tool sets it for scheduled facts.
  std::string typeHint;
};

struct FormationResult
{
  int64_t factId = 0;
  int64_t subjectEntityId = 0;
  std::string predicate;
  std::string canonical;
  bool superseded = false;
  std::string source;
};

struct MemoryFormationDeps
{
  SqliteGraph& graph;
  EntityResolver& resolver;
  const RuleParser& ruleParser;
};

struct EntityResolveInput
{
  std::string surface;
  std::string lang;
  std::string kindHint = "thing";
};

class MemoryFormation
{
public:
  explicit MemoryFormation(const MemoryFormationDeps& deps)
      : graph_(deps.graph), resolver_(deps.resolver),
        ruleParser_(deps.ruleParser)
  {
  }

  void setExtractor(const extract::IFactExtractor* extractor)
  {
    extractor_ = extractor;
  }

  std::optional<FormationResult>
  observe(const Observation& obs,
          const std::optional<tools::ToolCall>& toolCall = std::nullopt);

  int64_t resolveOrCreateEntity(const EntityResolveInput& input);

private:
  SqliteGraph& graph_;
  EntityResolver& resolver_;
  const RuleParser& ruleParser_;
  const extract::IFactExtractor* extractor_ = nullptr;
};
