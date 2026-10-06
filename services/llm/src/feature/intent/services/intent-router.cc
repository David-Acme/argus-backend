#include "intent-router.hxx"

#include <phrase/phrase-catalog.hxx>
#include <phrase/rule-parser.hxx>
#include <text/text-norm.hxx>

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>
#include <vector>

namespace
{

constexpr std::string_view kOpenerFill = " ,.";
constexpr std::string_view kWordBoundary = " ,.;:?!";

constexpr std::array<std::string_view, 21> kReminderAsks{
    "recuerdame", "recuerdanos", "me recuerdas", "me recuerdes", "me recordaras",
    "acuerdame", "avisame", "me avises", "me avisas", "me avisaras",
    "despiertame", "alarma", "temporizador", "remind me", "remind us",
    "let me know", "wake me", "alarm", "timer", "ping me", "tell me when"};

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

bool IntentRouter::asksToBeReminded(const std::string& text) const
{
  const std::string normalized = intent::normalizeInput(text);
  return std::ranges::any_of(kReminderAsks, [&normalized](std::string_view ask) {
    return normalized.find(ask) != std::string::npos;
  });
}

intent::ToolIntent IntentRouter::factOrReminder(const std::string& text,
                                                const std::string& lang) const
{
  if (!asksToBeReminded(text) || !recurrent_ || recurrent_(text, lang))
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

std::optional<IntentRouter::RuleProposal>
IntentRouter::propose(const std::string& text, const std::string& lang) const
{
  const RuleParser parser(catalog_);
  const RuleParseInput input{.text = text, .lang = lang};

  if (parser.parse(input).has_value())
    return RuleProposal{.intent = factOrReminder(text, lang),
                        .source = intent::DecisionSource::Trigger};
  if (parser.isCancellation(input))
    return RuleProposal{.intent = intent::ToolIntent::MemoryForget,
                        .source = intent::DecisionSource::Cancellation};
  if (parser.parseStatement(input))
    return RuleProposal{.intent = factOrReminder(text, lang),
                        .source = intent::DecisionSource::Statement};
  if (recallMarkerOpens(text_norm::whitespace(text, true), lang))
    return RuleProposal{.intent = intent::ToolIntent::MemoryRecall,
                        .source = intent::DecisionSource::RecallMarker};
  return std::nullopt;
}

intent::IntentDecision IntentRouter::decide(const std::string& text,
                                            const std::string& lang) const
{
  const std::optional<RuleProposal> proposal = propose(text, lang);
  const auto byRules = [&proposal](float score) {
    return intent::IntentDecision{.intent = proposal->intent,
                                  .score = score,
                                  .fromRules = true,
                                  .confident = true,
                                  .source = proposal->source};
  };

  const bool explicitTrigger =
      proposal && proposal->source == intent::DecisionSource::Trigger;
  const auto hits = model_.score(intent::normalizeInput(text));
  if (!model_.isLoaded() || hits.empty())
    return explicitTrigger ? byRules(1.0F) : intent::IntentDecision{};

  const intent::IntentHit& top = hits.front();
  const float runner = hits.size() > 1 ? hits[1].score : 0.0F;
  intent::IntentDecision decision{
      .intent = top.intent, .score = top.score, .margin = top.score - runner};

  if (top.score >= kThreshold && decision.margin >= kMargin) {
    if (explicitTrigger)
      return byRules(1.0F);
    if (top.intent == intent::ToolIntent::MemorySave && runner > 0.0F &&
        hits[1].intent == intent::ToolIntent::ReminderSet) {
      decision.intent = factOrReminder(text, lang);
    }
    decision.confident = true;
    return decision;
  }

  if (explicitTrigger)
    return byRules(1.0F);
  if (proposal && top.intent == proposal->intent && top.score >= kAgreeFloor)
    return byRules(top.score);
  return decision;
}
