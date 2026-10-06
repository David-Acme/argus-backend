#include "tool-grounding.hxx"

#include <feature/llm/services/tools/app-command.hxx>
#include <feature/llm/services/tools/module-offer.hxx>
#include <feature/llm/services/tools/spoken-intent.hxx>

#include <algorithm>
#include <array>
#include <string_view>

namespace
{
constexpr std::string_view kGuardModeTool = "app.set_guard_mode";
constexpr std::string_view kEnableTool = "modules.enable";
constexpr std::string_view kRequestTool = "modules.request";
constexpr std::string_view kConfirmation = "confirmation";

struct Label
{
  std::string_view value;
  std::string_view es;
  std::string_view en;
};

constexpr std::array<Label, 4> kModeWords{{{.value = "home", .es = "casa", .en = "home"},
                                           {.value = "night", .es = "noche", .en = "night"},
                                           {.value = "away", .es = "fuera", .en = "away"},
                                           {.value = "armed", .es = "armado", .en = "armed"}}};

std::string modeWord(const tools::ToolCall& call)
{
  std::string value = call.arguments.get("mode", "").asString();
  const auto found = std::ranges::find(kModeWords, std::string_view(value), &Label::value);
  if (found == kModeWords.end())
    return value;
  return std::string(call.context.lang == "en" ? found->en : found->es);
}

bool spokenGuardMode(const tools::ToolCall& call)
{
  const std::string requested = call.arguments.get("mode", "").asString();
  if (requested == "armed")
    return true;
  if (namesGuardMode(call.context.utterance, requested))
    return true;
  const auto heard = appCommandFor(call.context.utterance);
  return heard && heard->name == call.name && heard->arguments.get("mode", "").asString() == requested;
}

struct Refused
{
  const tools::ToolCall& call;
  std::string code;
  std::string text;
};

tools::ToolResult refusal(Refused refused)
{
  tools::ToolResult result;
  result.tool = refused.call.name;
  result.code = std::move(refused.code);
  result.output = std::move(refused.text);
  return result;
}

bool carries(const argus::mcp::ToolSpec& spec)
{
  return spec.inputSchema["properties"].isMember(std::string(kConfirmation));
}
}

std::optional<int64_t> ToolGrounding::turnOf(const std::map<Key, int64_t>& book, const Key& key) const
{
  const std::scoped_lock lock(mutex_);
  const auto found = book.find(key);
  if (found == book.end())
    return std::nullopt;
  return found->second;
}

std::optional<tools::ToolResult> ToolGrounding::refuseConfirmation(const GroundingInput& input) const
{
  const tools::ToolCall& call = input.call;
  const bool english = call.context.lang == "en";
  const Json::Value& token = call.arguments[std::string(kConfirmation)];
  if (!input.spec.annotations.destructive || !carries(input.spec) || !token.isString() || token.asString().empty())
    return std::nullopt;
  const auto previewed = turnOf(previews_, {call.context.userId, input.spec.name});
  const bool later = previewed && (call.context.turn == 0 || *previewed != call.context.turn);
  if (later && spoken_intent::affirms(call.context.utterance))
    return std::nullopt;
  return refusal({.call = call,
                  .code = "needs_spoken_yes",
                  .text = english ? "Not done: the user has not confirmed. Ask them clearly and, when they say yes in "
                                    "their next message, repeat the call with the code."
                                  : "No lo hice: el usuario no lo ha confirmado. Pregúntaselo con claridad y, cuando "
                                    "diga que sí en su siguiente mensaje, repite la llamada con el código."});
}

std::optional<tools::ToolResult> ToolGrounding::refuseModuleChange(const GroundingInput& input) const
{
  const tools::ToolCall& call = input.call;
  const bool enabling = input.spec.name == kEnableTool;
  const bool requesting = input.spec.name == kRequestTool;
  if (!enabling && !requesting)
    return std::nullopt;
  const std::string id = call.arguments.get("module", "").asString();
  const ModuleFlag* module = moduleNamed(input.audience.modules, id);
  const bool english = call.context.lang == "en";
  if (module != nullptr) {
    const spoken_intent::ModuleMention mention{.utterance = call.context.utterance, .module = *module};
    const bool explicitAsk = enabling ? spoken_intent::asksToEnable(mention) : spoken_intent::asksToRequest(mention);
    const auto offered = turnOf(offers_, {call.context.userId, id});
    const bool accepted = offered && (call.context.turn == 0 || *offered != call.context.turn) &&
                          spoken_intent::affirms(call.context.utterance);
    if (explicitAsk || accepted)
      return std::nullopt;
  }
  return refusal({.call = call,
                  .code = "needs_spoken_yes",
                  .text = english ? "Not done: the user has not asked for it or accepted it yet. Offer it and wait for "
                                    "their yes."
                                  : "No lo hice: el usuario aún no lo ha pedido ni lo ha aceptado. Ofrécelo y espera "
                                    "su sí."});
}

std::optional<tools::ToolResult> ToolGrounding::refuse(const GroundingInput& input) const
{
  const tools::ToolCall& call = input.call;
  if (input.spec.name == kGuardModeTool && !spokenGuardMode(call)) {
    const std::string mode = modeWord(call);
    return refusal({.call = call,
                    .code = "needs_spoken_words",
                    .text = call.context.lang == "en"
                                ? "Not changed. Lowering the guard needs the user's own words: ask them to say "
                                  "\"set the guard to " + mode + "\"."
                                : "No lo he cambiado. Bajar la vigilancia necesita que el usuario lo diga: "
                                  "pídele que diga \"pon la vigilancia en modo " + mode + "\"."});
  }
  if (auto refused = refuseConfirmation(input))
    return refused;
  return refuseModuleChange(input);
}

void ToolGrounding::remember(const GroundingInput& input, const tools::ToolResult& result) const
{
  const std::scoped_lock lock(mutex_);
  if (result.ok && result.data["needsConfirmation"].isBool() && result.data["needsConfirmation"].asBool())
    previews_[{input.call.context.userId, input.spec.name}] = input.call.context.turn;
  const bool changesModule = input.spec.name == kEnableTool || input.spec.name == kRequestTool;
  if (changesModule && result.ok)
    offers_.erase({input.call.context.userId, input.call.arguments.get("module", "").asString()});
  if (result.code == "module_inactive" && result.data["module"].isString())
    offers_[{input.call.context.userId, result.data["module"].asString()}] = input.call.context.turn;
}
