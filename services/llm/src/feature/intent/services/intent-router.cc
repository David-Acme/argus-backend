#include "intent-router.hxx"

#include <phrase/phrase-catalog.hxx>
#include <phrase/rule-parser.hxx>
#include <text/text-norm.hxx>

#include <algorithm>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

constexpr std::string_view kOpenerFill = " ,.";
constexpr std::string_view kWordBoundary = " ,.;:?!";

bool boundaryAt(std::string_view lowered, size_t index)
{
  return index >= lowered.size() ||
         kWordBoundary.find(lowered[index]) != std::string_view::npos;
}

}

IntentRouter::IntentRouter(IntentRouterInput input)
    : catalog_(input.catalog), model_(input.model),
      recurrent_(std::move(input.recurrent))
{
}

intent::ToolIntent IntentRouter::factOrReminder(const std::string& text,
                                                const std::string& lang) const
{
  if (!recurrent_ || recurrent_(text, lang))
    return intent::ToolIntent::MemorySave;
  return intent::ToolIntent::ReminderSet;
}

bool IntentRouter::recallMarkerOpens(const std::string& lowered,
                                     const std::string& lang) const
{
  const std::vector<PhraseHit> hits = catalog_.match(lowered, lang);
  return std::ranges::any_of(hits, [&lowered](const PhraseHit& hit) {
    const std::string_view before(lowered.data(), hit.begin);
    return hit.kind == PhraseKind::RecallMarker &&
           before.find_first_not_of(kOpenerFill) == std::string_view::npos &&
           boundaryAt(lowered, hit.end);
  });
}

intent::IntentDecision IntentRouter::decide(const std::string& text,
                                            const std::string& lang) const
{
  intent::IntentDecision decision;
  const RuleParser parser(catalog_);
  const RuleParseInput input{.text = text, .lang = lang};

  const bool triggered = parser.parse(input).has_value();
  if (!triggered && parser.isCancellation(input)) {
    return {.intent = intent::ToolIntent::MemoryForget,
            .fromRules = true,
            .confident = true};
  }
  if (triggered || parser.parseStatement(input)) {
    const intent::ToolIntent kind = factOrReminder(text, lang);
    return {.intent = kind,
            .score = 1.0F,
            .fromRules = true,
            .confident = true};
  }
  if (recallMarkerOpens(text_norm::whitespace(text, true), lang)) {
    return {.intent = intent::ToolIntent::MemoryRecall,
            .fromRules = true,
            .confident = true};
  }

  const auto hits = model_.score(intent::normalizeInput(text));
  if (!model_.isLoaded() || hits.empty())
    return decision;

  const intent::IntentHit& top = hits.front();
  const float runner = hits.size() > 1 ? hits[1].score : 0.0F;
  decision.intent = top.intent;
  decision.score = top.score;
  decision.margin = top.score - runner;
  if (top.score < kThreshold || decision.margin < kMargin)
    return decision;

  if (top.intent == intent::ToolIntent::MemorySave && runner > 0.0F &&
      hits[1].intent == intent::ToolIntent::ReminderSet) {
    decision.intent = factOrReminder(text, lang);
  }
  decision.confident = true;
  return decision;
}
