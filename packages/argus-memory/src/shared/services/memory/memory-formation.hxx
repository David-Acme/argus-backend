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
  // An upstream classifier already decided this turn is a save, so the
  // sentence needs neither an explicit trigger nor a successful extraction to
  // be stored: 39.5% of real memory_save utterances carry no rule clause, and
  // dropping them would throw away exactly what the user asked to keep.
  bool decided = false;
  // Forces the stored fact type the rule path would otherwise infer; the
  // reminder tool sets it so a scheduled fact is not filed as an attribute.
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
