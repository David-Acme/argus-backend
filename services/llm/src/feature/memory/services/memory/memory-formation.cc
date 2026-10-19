#include "memory-formation.hxx"

#include <algorithm>
#include <feature/memory/services/memory/memory-scope-classifier.hxx>
#include <array>
#include <string_view>

#include <drogon/drogon.h>
#include <feature/memory/services/extract/temporal-resolver.hxx>
#include <phrase/rule-parser.hxx>
#include <feature/memory/services/memory/sqlite-graph.hxx>
#include <text/text-norm.hxx>

namespace
{

constexpr std::string_view kNotePredicate = "nota";

bool mentionsFirstPerson(const std::string& text)
{
  const std::string norm = " " + text_norm::whitespace(text) + " ";
  static constexpr std::array<std::string_view, 4> kMarkers{" me ", " yo ",
                                                            " mi ", " mis "};
  return std::ranges::any_of(kMarkers, [&norm](std::string_view marker) {
    return norm.find(marker) != std::string::npos;
  });
}

std::string factTypeFromMemoryType(const std::string& type)
{
  if (type == "persona")
    return "persona";
  if (type == "instruction")
    return "instruction";
  return "attribute";
}

struct TimeProbe
{
  const std::string& text;
  const std::string& lang;
};

bool namesATime(const TimeProbe& probe)
{
  static const TemporalResolver resolver;
  return resolver
             .resolve({.lang = probe.lang.empty() ? "es" : probe.lang,
                       .normalized = TemporalResolver::normalize(probe.text)})
             .kind != extract::TemporalKind::None;
}

}

int64_t
MemoryFormation::resolveOrCreateEntity(const EntityResolveInput& input)
{
  const std::string& surface = input.surface;
  const std::string& lang = input.lang;
  const std::string& kindHint = input.kindHint;
  if (surface.empty())
    return 0;

  for (const auto& hit : resolver_.resolve(surface)) {
    if (hit.entityId > 0)
      return hit.entityId;
  }

  const std::string norm = text_norm::whitespace(surface);
  if (norm.empty())
    return 0;

  std::optional<int64_t> entityId;
  {
    std::scoped_lock lock(graph_.mutex());
    entityId =
        graph_.resolveEntity({.surface = surface, .norm = norm, .lang = lang});
  }
  if (entityId)
    return *entityId;

  std::string kind = kindHint;
  std::string personFrame = "none";
  std::optional<int64_t> personId;
  for (const auto& hit : resolver_.resolve(surface)) {
    if (hit.kind == "person" || hit.kind == "device" || hit.kind == "place") {
      kind = hit.kind;
      personFrame = hit.personFrame.empty() ? "none" : hit.personFrame;
      if (hit.catalog == "person")
        personId = hit.catalogId;
      break;
    }
  }

  int64_t created = 0;
  {
    std::scoped_lock lock(graph_.mutex());
    created = graph_.createEntity(
        {.kind = kind, .canonical = norm, .lang = lang, .personId = personId});
    if (created > 0) {
      graph_.addAlias({.entityId = created,
                       .surface = surface,
                       .norm = norm,
                       .lang = lang,
                       .personFrame = personFrame,
                       .confidence = 0.8F});
    }
  }
  if (created > 0)
    resolver_.build();
  return created;
}

std::optional<FormationResult>
MemoryFormation::observe(const Observation& obs,
                         const std::optional<tools::ToolCall>& toolCall)
{
  if (obs.text.empty())
    return std::nullopt;
  if (obs.text.find("<memor") != std::string::npos)
    return std::nullopt;
  resolver_.build();

  FormationResult result;
  std::string subjectSurface;
  std::string predicate;
  std::string value;
  std::string factType;
  std::string naturalClause;
  int priority = 85;
  float confidence = 0.8F;

  const Json::Value* toolArgs = nullptr;
  if (toolCall && toolCall->name == "memory.remember")
    toolArgs = &toolCall->arguments;
  if (toolArgs) {
    subjectSurface = toolArgs->get("subject", "").asString();
    predicate = toolArgs->get("predicate", "").asString();
    value = toolArgs->get("value", "").asString();
  }
  const bool completeTriple = toolArgs && !subjectSurface.empty() &&
                              !predicate.empty() && !value.empty();
  bool askedToKeep = false;
  if (completeTriple) {
    factType = toolArgs->get("type", "attribute").asString();
    confidence = toolArgs->get("confidence", 0.8).asFloat();
    result.source = "tool";
  }
  else {
    const auto parsed = ruleParser_.parse({.text = obs.text, .lang = obs.lang});
    const auto statement =
        parsed
            ? std::nullopt
            : ruleParser_.parseStatement({.text = obs.text, .lang = obs.lang});
    if (!parsed && !statement && !obs.decided)
      return std::nullopt;

    const RuleParseResult salientGate{.type = MemoryType::Persona,
                                      .content = obs.text,
                                      .priority = 85};
    const RuleParseResult& gate =
        parsed ? *parsed : (statement ? *statement : salientGate);
    const std::string clause =
        ruleParser_.stripFillers({.text = gate.content, .lang = obs.lang});
    if (clause.empty())
      return std::nullopt;

    const bool timedRequest = obs.decided && obs.typeHint == "schedule" && !statement.has_value() &&
                              !ruleParser_.isCommand({.text = obs.text, .lang = obs.lang}) &&
                              namesATime({.text = obs.text, .lang = obs.lang});
    const bool explicitTrigger = parsed.has_value() || statement.has_value() || timedRequest;
    askedToKeep = parsed.has_value() || timedRequest;

    if (!parsed.has_value() &&
        ruleParser_.isQuestion({.text = obs.text, .lang = obs.lang}))
      return std::nullopt;

    std::vector<extract::ExtractedFact> extracted;
    if (extractor_ && explicitTrigger)
      extractor_->extract({.clause = clause,
                           .lang = obs.lang,
                           .userId = obs.userId},
                          extracted);

    const bool usable = !extracted.empty() && [&] {
      const auto& first = extracted.front();
      return !first.subject.empty() && !first.predicate.empty() &&
             (!first.value.empty() || !first.when.surface.empty()) &&
             !ruleParser_.isFiller(first.predicate, obs.lang) &&
             !ruleParser_.isFiller(first.subject, obs.lang);
    }();
    if (usable) {
      const auto& first = extracted.front();
      subjectSurface = first.subject;
      predicate = first.predicate;
      value = first.value.empty() ? first.when.surface : first.value;
      factType = first.factType.empty()
                     ? factTypeFromMemoryType(memoryTypeToString(gate.type))
                     : first.factType;
      confidence = first.confidence;
      priority = gate.priority;
      result.source = "lexicon";
    }
    else {
      if (extractor_ && !explicitTrigger)
        return std::nullopt;
      value = clause;
      predicate = std::string(kNotePredicate);
      factType = factTypeFromMemoryType(memoryTypeToString(gate.type));
      priority = gate.priority;
      result.source = "rule";
      for (const auto& hit : resolver_.resolve(clause)) {
        if (hit.kind == "person" || hit.kind == "device" ||
            hit.kind == "place") {
          subjectSurface = hit.surface;
          break;
        }
      }
    }
    naturalClause = clause;
  }

  if (!obs.typeHint.empty())
    factType = obs.typeHint;
  if (subjectSurface.empty() && (askedToKeep || mentionsFirstPerson(obs.text)))
    subjectSurface = "usuario";
  if (subjectSurface.empty())
    return std::nullopt;

  const int64_t entityId =
      resolveOrCreateEntity({.surface = subjectSurface, .lang = obs.lang});
  if (entityId == 0)
    return std::nullopt;
  const MemoryScope scope = classifyMemoryScope({.text = obs.text, .lang = obs.lang});
  const std::string scopeName = memoryScopeToString(scope);
  const int64_t scopeRef = scope == MemoryScope::Household ? 0 : obs.userId;

  const std::string canonical = text_norm::whitespace(
      naturalClause.empty() ? predicate + " " + value : naturalClause);
  if (canonical.empty())
    return std::nullopt;

  std::optional<int64_t> sourceId;
  {
    std::scoped_lock lock(graph_.mutex());
    const int64_t id = graph_.createSource(
        {.channel = obs.channel, .turnRef = obs.sessionId, .at = obs.at});
    if (id > 0)
      sourceId = id;
  }

  const int64_t factId = [&] {
    std::scoped_lock lock(graph_.mutex());
    return graph_.upsertFact({.entityId = entityId,
                              .predicate = predicate,
                              .value = value,
                              .canonical = canonical,
                              .type = factType,
                              .priority = priority,
                              .confidence = confidence,
                              .lang = obs.lang,
                              .scope = scopeName,
                              .refId = scopeRef,
                              .now = obs.at,
                              .sourceId = sourceId,
                              .supersedes = predicate != kNotePredicate});
  }();
  if (factId == 0)
    return std::nullopt;
  result.factId = factId;
  result.subjectEntityId = entityId;
  result.predicate = predicate;
  result.canonical = canonical;
  return result;
}
