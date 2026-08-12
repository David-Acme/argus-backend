#include "memory-formation.hxx"

#include <drogon/drogon.h>
#include <shared/services/memory/rule-parser.hxx>
#include <shared/services/memory/sqlite-graph.hxx>
#include <shared/utils/text-norm/text-norm.hxx>

namespace
{

bool mentionsFirstPerson(const std::string& text)
{
  const std::string norm = text_norm::whitespace(text);
  const std::vector<std::string> markers = {" me ", " yo ", " mi ", " mis "};
  for (const auto& marker : markers) {
    if (norm.find(marker) != std::string::npos)
      return true;
  }
  return norm == "me" || norm == "yo" || norm == "mi";
}

std::string factTypeFromMemoryType(const std::string& type)
{
  if (type == "persona")
    return "persona";
  if (type == "instruction")
    return "instruction";
  return "attribute";
}

} // namespace

int64_t MemoryFormation::resolveOrCreateEntity(const std::string& surface,
                                               const std::string& lang,
                                               const std::string& kindHint)
{
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
  resolver_.build();

  FormationResult result;
  std::string subjectSurface;
  std::string predicate;
  std::string value;
  std::string factType;
  std::string naturalClause;
  int priority = 85;
  float confidence = 0.8F;

  if (toolCall && toolCall->name == "memory.remember") {
    const Json::Value& args = toolCall->arguments;
    subjectSurface = args.get("subject", "").asString();
    predicate = args.get("predicate", "").asString();
    value = args.get("value", "").asString();
    factType = args.get("type", "persona").asString();
    confidence = args.get("confidence", 0.8).asFloat();
    result.source = "tool";
  }
  else {
    const auto parsed = ruleParser_.parse({.text = obs.text, .lang = obs.lang});
    const auto statement =
        parsed
            ? std::nullopt
            : ruleParser_.parseStatement({.text = obs.text, .lang = obs.lang});
    if (!parsed && !statement && !obs.salient)
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

    const bool explicitTrigger = parsed.has_value() || statement.has_value();

    if (!explicitTrigger &&
        ruleParser_.isQuestion({.text = obs.text, .lang = obs.lang}))
      return std::nullopt;

    std::vector<extract::ExtractedFact> extracted;
    if (extractor_)
      extractor_->extract({.clause = clause,
                           .lang = obs.lang,
                           .userId = obs.userId,
                           .requireModel = !explicitTrigger,
                           .allowModel = obs.allowModel},
                          extracted);

    if (!extracted.empty()) {
      const auto& first = extracted.front();
      if (first.subject.empty() || first.predicate.empty() ||
          (first.value.empty() && first.when.surface.empty()))
        return std::nullopt;
      if (ruleParser_.isFiller(first.predicate, obs.lang) ||
          ruleParser_.isFiller(first.subject, obs.lang))
        return std::nullopt;
      subjectSurface = first.subject;
      predicate = first.predicate;
      value = first.value.empty() ? first.when.surface : first.value;
      factType = first.factType.empty()
                     ? factTypeFromMemoryType(memoryTypeToString(gate.type))
                     : first.factType;
      confidence = first.confidence;
      priority = gate.priority;
      result.source =
          first.tier == extract::ExtractTier::Lexicon ? "lexicon" : "model";
    }
    else {
      if (extractor_)
        return std::nullopt;
      value = clause;
      predicate = "nota";
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
    if (explicitTrigger || result.source == "rule") {
      naturalClause = clause;
    }
    else {
      naturalClause = subjectSurface + " " + predicate;
      if (!value.empty())
        naturalClause += " " + value;
      if (!extracted.empty() && !extracted.front().when.surface.empty() &&
          value != extracted.front().when.surface)
        naturalClause += " " + extracted.front().when.surface;

      const std::string lowered = text_norm::whitespace(clause);
      const std::string needle = text_norm::whitespace(naturalClause);
      const size_t at = lowered.find(needle);
      if (at != std::string::npos) {
        static constexpr std::string_view kMarkers[] = {"a ", "al ", "a mi ",
                                                        "a tu ", "a la "};
        for (const auto marker : kMarkers) {
          if (at < marker.size())
            continue;
          if (lowered.compare(at - marker.size(), marker.size(), marker) != 0)
            continue;
          naturalClause =
              lowered.substr(at - marker.size(), marker.size() + needle.size());
          break;
        }
      }
    }
  }

  if (subjectSurface.empty() && mentionsFirstPerson(obs.text))
    subjectSurface = "usuario";
  if (subjectSurface.empty())
    return std::nullopt;

  const int64_t entityId = resolveOrCreateEntity(subjectSurface, obs.lang);
  if (entityId == 0)
    return std::nullopt;

  const std::string canonical = text_norm::whitespace(
      naturalClause.empty() ? predicate + " " + value : naturalClause);
  if (canonical.empty())
    return std::nullopt;

  std::optional<int64_t> sourceId;
  {
    std::scoped_lock lock(graph_.mutex());
    const int64_t id = graph_.createSource(obs.channel, obs.sessionId, obs.at);
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
                              .scope = "user",
                              .refId = obs.userId,
                              .now = obs.at,
                              .sourceId = sourceId});
  }();
  if (factId == 0)
    return std::nullopt;

  result.factId = factId;
  result.subjectEntityId = entityId;
  result.predicate = predicate;
  result.canonical = canonical;
  return result;
}
