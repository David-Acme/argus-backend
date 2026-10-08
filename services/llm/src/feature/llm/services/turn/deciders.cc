#include "deciders.hxx"

#include <feature/llm/services/tools/app-command.hxx>
#include <feature/llm/services/tools/module-command.hxx>

#include <string>
#include <utility>

namespace turn
{

namespace
{
constexpr double kRuleConfidence = 1.0;

std::optional<tools::ToolCall> memoryCall(intent::ToolIntent decided, const std::string& utterance)
{
  tools::ToolCall call;
  call.arguments = Json::Value(Json::objectValue);
  switch (decided) {
    case intent::ToolIntent::MemorySave:
      call.name = "memory.remember";
      call.arguments["text"] = utterance;
      break;
    case intent::ToolIntent::MemoryRecall:
      call.name = "memory.recall";
      call.arguments["query"] = utterance;
      break;
    case intent::ToolIntent::ReminderSet:
      call.name = "memory.remind";
      call.arguments["text"] = utterance;
      break;
    case intent::ToolIntent::MemoryForget:
      call.name = "memory.forget";
      call.arguments["query"] = utterance;
      break;
    default:
      return std::nullopt;
  }
  return call;
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

std::optional<Candidate> RouterDecider::decide(const DecideInput& input) const
{
  if (router_ == nullptr)
    return std::nullopt;
  const std::string utterance(input.utterance);
  const intent::IntentDecision decision = router_->decide(utterance, std::string(input.lang));
  auto call = memoryCall(decision.intent, utterance);
  if (!call || !isOffered(input, call->name))
    return std::nullopt;
  std::optional<Pick> second;
  if (!decision.fromRules)
    if (auto other = memoryCall(decision.runnerUp, utterance); other && isOffered(input, other->name))
      second = Pick{.tool = other->name,
                    .arguments = other->arguments,
                    .fill = {},
                    .confidence = static_cast<double>(decision.runnerUpScore),
                    .source = std::string(intent::toolIntentToString(decision.runnerUp)) + ", model"};
  return Candidate{.tool = call->name,
                   .arguments = call->arguments,
                   .fill = {},
                   .confidence = decision.fromRules ? kRuleConfidence : static_cast<double>(decision.score),
                   .source = std::string(intent::toolIntentToString(decision.intent)) +
                             (decision.fromRules ? ", rules" : ", model"),
                   .decider = std::string(id()),
                   .exact = decision.fromRules,
                   .confident = decision.confident,
                   .runnerUp = std::move(second)};
}

std::optional<Candidate> FirstOf::decide(const DecideInput& input) const
{
  for (const Decider* decider : deciders_)
    if (auto candidate = decider->decide(input))
      return candidate;
  return std::nullopt;
}

}
