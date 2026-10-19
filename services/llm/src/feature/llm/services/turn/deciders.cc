#include "deciders.hxx"

#include <feature/llm/services/tools/app-command.hxx>
#include <feature/llm/services/tools/module-command.hxx>
#include <feature/memory/services/extract/call-time.hxx>
#include <feature/memory/services/extract/repeat-rule.hxx>
#include <feature/memory/services/extract/temporal-resolver.hxx>
#include <phrase/rule-parser.hxx>
#include <text/text-norm.hxx>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace turn
{

namespace
{
constexpr double kRuleConfidence = 1.0;

constexpr std::array<std::string_view, 21> kReminderAsks{
    "recuerdame", "recuerdanos", "me recuerdas", "me recuerdes", "me recordaras",
    "acuerdame", "avisame", "me avises", "me avisas", "me avisaras",
    "despiertame", "alarma", "temporizador", "remind me", "remind us",
    "let me know", "wake me", "alarm", "timer", "ping me", "tell me when"};

bool asksToBeReminded(std::string_view utterance)
{
  const std::string folded =
      text_norm::whitespace(text_norm::stripAccents(text_norm::intent(std::string(utterance))), true);
  return std::ranges::any_of(kReminderAsks,
                             [&folded](std::string_view ask) { return folded.find(ask) != std::string::npos; });
}

bool namesOneInstant(std::string_view utterance, std::string_view lang)
{
  static const TemporalResolver resolver;
  const extract::TemporalValue when =
      resolver.resolve({.lang = lang.empty() ? std::string_view("es") : lang, .normalized = TemporalResolver::normalize(utterance)});
  return when.kind != extract::TemporalKind::None && when.recur == extract::Recurrence::None;
}

struct RepeatAsk
{
  std::string_view utterance;
  std::string_view lang;
  bool parsed{false};
};

bool asksToRepeat(const RepeatAsk& ask)
{
  const RepeatReading reading = repeat_rule::read({.text = ask.utterance, .lang = ask.lang});
  if (!reading.mentioned)
    return false;
  if (!reading.repeat || !ask.parsed)
    return true;
  return call_time::clockOf({.text = ask.utterance, .part = reading.part}).has_value();
}
}

std::optional<Candidate> RuleDecider::decide(const DecideInput& input) const
{
  if (auto family = module_command::commandFor({.utterance = input.utterance, .modules = input.modules});
      family && isOffered(input, family->tool))
    return Candidate{.tool = family->tool,
                     .arguments = family->arguments,
                     .fill = family->fill,
                     .confidence = kRuleConfidence,
                     .source = "module command",
                     .decider = std::string(id()),
                     .exact = true,
                     .runnerUp = std::nullopt};
  const std::string utterance(input.utterance);
  if (auto command = appCommandFor(utterance); command && isOffered(input, command->name))
    return Candidate{.tool = command->name,
                     .arguments = command->arguments,
                     .fill = {},
                     .confidence = kRuleConfidence,
                     .source = "app command",
                     .decider = std::string(id()),
                     .exact = true,
                     .runnerUp = std::nullopt};
  return std::nullopt;
}

TriggerDecider::TriggerDecider()
{
  catalog_.build();
}

std::optional<Candidate> TriggerDecider::decide(const DecideInput& input) const
{
  const std::string utterance(input.utterance);
  const std::string lang(input.lang);
  const RuleParser parser(catalog_);
  const RuleParseInput parseInput{.text = utterance, .lang = lang};
  const bool parsed = parser.parse(parseInput).has_value();
  const bool repeating = asksToBeReminded(utterance) && !parser.isQuestion(parseInput) &&
                         asksToRepeat({.utterance = utterance, .lang = lang, .parsed = parsed});
  if (!parsed && !repeating)
    return std::nullopt;
  const bool reminder = repeating || (asksToBeReminded(utterance) && namesOneInstant(utterance, lang));
  const std::string tool = reminder ? "memory.remind" : "memory.remember";
  if (!isOffered(input, tool))
    return std::nullopt;
  Json::Value arguments(Json::objectValue);
  arguments["text"] = utterance;
  return Candidate{.tool = tool,
                   .arguments = std::move(arguments),
                   .fill = {},
                   .confidence = kRuleConfidence,
                   .source = reminder ? "reminder_set, trigger" : "memory_save, trigger",
                   .decider = std::string(id()),
                   .exact = true,
                   .confident = true,
                   .runnerUp = std::nullopt};
}

std::optional<Candidate> FirstOf::decide(const DecideInput& input) const
{
  for (const Decider* decider : deciders_)
    if (auto candidate = decider->decide(input))
      return candidate;
  return std::nullopt;
}

}
