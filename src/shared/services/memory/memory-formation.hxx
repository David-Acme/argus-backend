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

class MemoryFormation
{
public:
  MemoryFormation(SqliteGraph& graph, EntityResolver& resolver,
                  const RuleParser& ruleParser)
      : graph_(graph), resolver_(resolver), ruleParser_(ruleParser)
  {
  }

  void setExtractor(const extract::IFactExtractor* extractor)
  {
    extractor_ = extractor;
  }

  std::optional<FormationResult>
  observe(const Observation& obs,
          const std::optional<tools::ToolCall>& toolCall = std::nullopt);

  int64_t resolveOrCreateEntity(const std::string& surface,
                                const std::string& lang,
                                const std::string& kindHint = "thing");

private:
  SqliteGraph& graph_;
  EntityResolver& resolver_;
  const RuleParser& ruleParser_;
  const extract::IFactExtractor* extractor_ = nullptr;
};
