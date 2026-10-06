#include "guard-tools.hxx"

#include <auth/tool-gate.hxx>
#include <mcp/loop-tool.hxx>
#include <mcp/schema.hxx>
#include <text/name-match.hxx>

#include <algorithm>
#include <array>
#include <string_view>
#include <utility>

namespace
{
namespace schema = argus::mcp::schema;

struct ModeLabel
{
  std::string_view value;
  std::string_view es;
  std::string_view en;
};

constexpr std::array<ModeLabel, 4> kModes{{{.value = "home", .es = "en casa", .en = "home"},
                                           {.value = "night", .es = "noche", .en = "night"},
                                           {.value = "away", .es = "fuera de casa", .en = "away"},
                                           {.value = "armed", .es = "máxima alerta", .en = "armed"}}};

std::string modeLabel(const argus::mcp::ToolInvocation& invocation)
{
  std::string value = invocation.arguments.get("mode", "").asString();
  const auto found = std::ranges::find(kModes, std::string_view(value), &ModeLabel::value);
  if (found == kModes.end())
    return value;
  return std::string(invocation.caller.lang == "en" ? found->en : found->es);
}

std::string listed(const std::vector<EnvironmentChoice>& places)
{
  std::string out;
  for (const auto& place : places)
    out += (out.empty() ? "" : ", ") + place.name;
  return out;
}

argus::mcp::ToolOutcome changed(const argus::mcp::ToolInvocation& invocation, const EnvironmentChoice* place)
{
  const bool english = invocation.caller.lang == "en";
  const std::string mode = modeLabel(invocation);
  argus::mcp::ToolOutcome outcome;
  Json::Value arguments(Json::objectValue);
  arguments["mode"] = invocation.arguments.get("mode", "").asString();
  if (place != nullptr) {
    arguments["environment"] = place->name;
    arguments["environmentId"] = place->id;
    outcome.text = english ? "The app set the guard mode of " + place->name + " to " + mode + "."
                           : "La app puso la vigilancia de " + place->name + " en modo " + mode + ".";
  }
  else {
    outcome.text = english ? "The app set the guard mode to " + mode + "."
                           : "La app puso la vigilancia en modo " + mode + ".";
  }
  outcome.structured = arguments;
  outcome.appAction = argus::mcp::AppAction{.name = "app.set_guard_mode", .arguments = std::move(arguments)};
  return outcome;
}

drogon::Task<argus::mcp::ToolOutcome> setGuardMode(EnvironmentCatalog catalog, argus::mcp::ToolInvocation invocation)
{
  const std::string asked = invocation.arguments.get("environment", "").asString();
  if (text_norm::folded(asked).empty())
    co_return changed(invocation, nullptr);
  const auto places = co_await catalog();
  std::vector<std::string> names;
  names.reserve(places.size());
  for (const auto& place : places)
    names.push_back(place.name);
  const auto match = text_norm::matchName(names, asked);
  const bool english = invocation.caller.lang == "en";
  if (match.kind == text_norm::NameMatchKind::Exact)
    co_return changed(invocation, &places.at(match.hits.front()));
  if (match.kind == text_norm::NameMatchKind::Ambiguous) {
    std::vector<EnvironmentChoice> candidates;
    candidates.reserve(match.hits.size());
    for (const size_t index : match.hits)
      candidates.push_back(places.at(index));
    co_return argus::mcp::toolFailure({.text = english ? "Which place do you mean? " + listed(candidates) + "."
                              : "¿A cuál lugar te refieres? " + listed(candidates) + ".", .code = "ambiguous_environment"});
  }
  co_return argus::mcp::toolFailure({.text = english ? "I could not find a place called " + asked + ". The places are: " + listed(places) + "."
                            : "No encuentro un lugar llamado " + asked + ". Los lugares son: " + listed(places) + ".", .code = "unknown_environment"});
}

}

std::shared_ptr<argus::mcp::McpServer> guardToolServer(const GuardToolsInput& input)
{
  auto server = std::make_shared<argus::mcp::McpServer>(
      argus::mcp::ServerIdentity{.name = "argus-guard", .version = "1", .instructions = ""});
  const EnvironmentCatalog catalog = input.catalog;
  server->add({.name = "app.set_guard_mode",
               .title = "",
               .description = "Cambia el modo de vigilancia: home al estar en casa, night al dormir, away al salir, "
                              "armed para máxima alerta. environment es el nombre del lugar (casa, restaurante, "
                              "oficina...) si el usuario nombra uno; vacío cambia todos",
               .inputSchema = schema::object(
                   {{.name = "mode", .schema = schema::choice({"home", "night", "away", "armed"}), .required = true},
                    {.name = "environment", .schema = schema::text(), .required = false}}),
               .annotations = {.idempotent = true},
               .module = "surveillance",
               .capability = "guard.mode.set"},
              argus::mcp::onLoop({.loop = input.loop, .handler = [catalog](const argus::mcp::ToolInvocation& invocation) {
                return setGuardMode(catalog, invocation);
              }}));
  server->setGate(tool_gate::capabilities());
  return server;
}
