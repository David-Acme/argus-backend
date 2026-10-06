#include "core-tools.hxx"

#include <auth/module-gate.hxx>
#include <feature/llm/services/tools/module-offer.hxx>
#include <mcp/schema.hxx>

#include <algorithm>
#include <array>
#include <string>
#include <string_view>
#include <utility>

namespace
{
namespace schema = argus::mcp::schema;

struct Screen
{
  std::string_view value;
  std::string_view es;
  std::string_view en;
  std::string_view module;
};

constexpr std::array<Screen, 8> kScreens{
    {{.value = "home", .es = "el inicio", .en = "the home screen", .module = "core"},
     {.value = "agenda", .es = "la agenda", .en = "the agenda", .module = "productivity"},
     {.value = "projects", .es = "los proyectos", .en = "the projects", .module = "productivity"},
     {.value = "cameras", .es = "las cámaras", .en = "the cameras", .module = "surveillance"},
     {.value = "security", .es = "la seguridad", .en = "security", .module = "surveillance"},
     {.value = "people", .es = "las personas", .en = "the people", .module = "core"},
     {.value = "settings", .es = "los ajustes", .en = "the settings", .module = "core"},
     {.value = "modules", .es = "los módulos", .en = "the modules", .module = "core"}}};

std::vector<std::string> screenNames()
{
  std::vector<std::string> names;
  names.reserve(kScreens.size());
  for (const auto& screen : kScreens)
    names.emplace_back(screen.value);
  return names;
}

tools::ToolContext contextOf(const argus::mcp::CallerContext& caller)
{
  tools::ToolContext context;
  context.userId = caller.userId;
  context.role = userRoleFromString(caller.role);
  context.lang = caller.lang.empty() ? "es" : caller.lang;
  context.sessionId = caller.sessionId;
  context.utterance = caller.utterance;
  context.decided = caller.decided;
  return context;
}

argus::mcp::ToolOutcome outcomeOf(const tools::ToolResult& result)
{
  argus::mcp::ToolOutcome outcome;
  outcome.isError = !result.ok;
  outcome.text = result.output;
  outcome.structured = result.data;
  if (!result.code.empty())
    outcome.structured["code"] = result.code;
  return outcome;
}

argus::mcp::ToolOutcome openScreen(const argus::mcp::ToolInvocation& invocation)
{
  const bool english = invocation.caller.lang == "en";
  const std::string value = invocation.arguments.get("screen", "").asString();
  const auto screen = std::ranges::find(kScreens, std::string_view(value), &Screen::value);
  argus::mcp::ToolOutcome outcome;
  if (screen == kScreens.end()) {
    outcome.isError = true;
    outcome.text = english ? "That screen does not exist." : "Esa pantalla no existe.";
    return outcome;
  }
  const ToolAudience audience{.role = userRoleFromString(invocation.caller.role),
                              .modules = moduleGate().snapshot()};
  if (!audience.modules.enabled(screen->module)) {
    outcome.isError = true;
    outcome.text = moduleOfferText({.audience = audience, .module = std::string(screen->module), .lang = english ? "en" : "es"});
    outcome.structured["code"] = "module_inactive";
    outcome.structured["module"] = std::string(screen->module);
    return outcome;
  }
  outcome.text = std::string(english ? "The app opened " : "La app abrió ") + std::string(english ? screen->en : screen->es) + ".";
  outcome.appAction = argus::mcp::AppAction{.name = "app.open", .arguments = invocation.arguments};
  return outcome;
}
}

argus::mcp::ToolSpec appOpenSpec()
{
  return {.name = "app.open",
          .title = "",
          .description = "Abre una sección de la app del usuario",
          .inputSchema = schema::object({{.name = "screen", .schema = schema::choice(screenNames()), .required = true},
                                         {.name = "module", .schema = schema::text(), .required = false}}),
          .annotations = {},
          .module = "core",
          .capability = "notifications.read"};
}

std::shared_ptr<argus::mcp::McpServer> coreToolServer(std::vector<tools::ToolDescriptor> descriptors)
{
  auto server = std::make_shared<argus::mcp::McpServer>(
      argus::mcp::ServerIdentity{.name = "argus-llm", .version = "1", .instructions = ""});
  for (auto& descriptor : descriptors) {
    if (!descriptor.handler)
      continue;
    server->addSync(descriptor.spec, [handler = std::move(descriptor.handler)](const argus::mcp::ToolInvocation& invocation) {
      tools::ToolCall call;
      call.name = invocation.name;
      call.arguments = invocation.arguments;
      call.context = contextOf(invocation.caller);
      return outcomeOf(handler(call));
    });
  }
  server->addSync(appOpenSpec(), openScreen);
  return server;
}
