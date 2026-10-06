#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <auth/capability.hxx>
#include <feature/llm/services/tools/app-command.hxx>
#include <feature/llm/services/tools/core-tools.hxx>
#include <feature/llm/services/tools/tool-executor.hxx>
#include <feature/llm/services/tools/tool-registry.hxx>
#include <feature/memory/services/memory/memory-tool-descriptors.hxx>
#include <mcp/schema.hxx>
#include <shared/vocabulary/tool-contracts.hxx>
#include "tool-stubs.hxx"

#include <algorithm>
#include <array>
#include <json/value.h>
#include <optional>
#include <string>
#include <vector>

namespace
{

struct Probe
{
  bool ran = false;
  std::string seen;
};

tools::ToolDescriptor probeDescriptor(const std::string& name, Probe& probe, const std::string& capability)
{
  return tool_stubs::stub({.name = name,
                           .capability = capability,
                           .handler = [&probe](const tools::ToolCall& call) {
                             probe.ran = true;
                             probe.seen = call.arguments.get("text", "").asString();
                             tools::ToolResult result;
                             result.ok = true;
                             result.output = "ran " + call.name;
                             result.tool = "probe";
                             result.data["seen"] = probe.seen;
                             return result;
                           }});
}

tools::ToolCall callFor(const std::string& name)
{
  tools::ToolCall call;
  call.name = name;
  call.arguments = Json::Value(Json::objectValue);
  call.context.userId = 7;
  call.context.utterance = "recuerdame algo";
  if (name == "memory.recall")
    call.arguments["query"] = "dentista";
  if (name == "memory.forget")
    call.arguments["query"] = "el dentista";
  return call;
}

tools::ToolCall bareCall(const std::string& name)
{
  tools::ToolCall call = callFor(name);
  call.arguments = Json::Value(Json::objectValue);
  return call;
}

ToolAudience audienceOf(UserRole role)
{
  return {.role = role, .modules = {}};
}

const tools::ToolDescriptor* declaredByMemory(const std::string& name)
{
  static const std::vector<tools::ToolDescriptor> declared = memoryToolDescriptors();
  for (const auto& descriptor : declared)
    if (descriptor.spec.name == name)
      return &descriptor;
  return nullptr;
}

void registerMemoryTools(ToolRegistry& registry)
{
  for (auto descriptor : memoryToolDescriptors()) {
    descriptor.handler = [](const tools::ToolCall&) { return tool_stubs::okResult("stored"); };
    registry.registerTool(std::move(descriptor));
  }
}

}

TEST_CASE("the executor dispatches a registered tool and refuses a name it does not hold")
{
  Probe probe;
  Probe never;
  ToolRegistry registry;
  registry.registerTool(probeDescriptor("probe.remember", probe, "memory.manage"));
  registry.registerTool(probeDescriptor("probe.forget", never, "settings.manage"));
  const ToolExecutor executor(registry);

  auto call = callFor("probe.remember");
  call.arguments["text"] = "dentista";
  const auto dispatched = executor.execute(call, audienceOf(UserRole::Resident));
  INFO("dispatch output: " << dispatched.output);
  CHECK(dispatched.ok);
  CHECK(probe.ran);
  CHECK(probe.seen == "dentista");
  CHECK_FALSE(never.ran);

  const auto unknown = executor.execute(callFor("probe.forgotten"), audienceOf(UserRole::Resident));
  CHECK_FALSE(unknown.ok);
  CHECK(unknown.output == "unknown tool: probe.forgotten");
  CHECK(unknown.code == "unknown_tool");
  CHECK_FALSE(never.ran);
}

TEST_CASE("a call the schema rejects never reaches the handler")
{
  ToolRegistry registry;
  registerMemoryTools(registry);
  const ToolExecutor executor(registry);

  const auto missing = executor.execute(bareCall("memory.recall"), audienceOf(UserRole::Resident));
  CHECK_FALSE(missing.ok);
  CHECK(missing.output == "missing required argument 'query'");
  CHECK(missing.code == "invalid_arguments");

  auto wrongEnum = callFor("memory.remember");
  wrongEnum.arguments["type"] = "inventado";
  const auto enumRefused = executor.execute(wrongEnum, audienceOf(UserRole::Resident));
  CHECK_FALSE(enumRefused.ok);
  CHECK(enumRefused.output == "argument 'type' has an invalid value");

  auto wrongType = callFor("memory.forget");
  wrongType.arguments["query"] = 42;
  const auto typeRefused = executor.execute(wrongType, audienceOf(UserRole::Resident));
  CHECK_FALSE(typeRefused.ok);
  CHECK(typeRefused.output == "argument 'query' must be a string");

  auto notObject = callFor("memory.remember");
  notObject.arguments = Json::Value("guardalo");
  const auto shapeRefused = executor.execute(notObject, audienceOf(UserRole::Resident));
  CHECK_FALSE(shapeRefused.ok);
  CHECK(shapeRefused.output == "arguments must be a JSON object");
}

TEST_CASE("a role that does not hold the capability never reaches the handler")
{
  Probe probe;
  ToolRegistry registry;
  registry.registerTool(probeDescriptor("probe.remember", probe, "memory.manage"));
  const ToolExecutor executor(registry);

  const auto refused = executor.execute(callFor("probe.remember"), audienceOf(UserRole::Guest));
  CHECK_FALSE(refused.ok);
  CHECK(refused.output == "permission denied for tool: probe.remember");
  CHECK(refused.code == "forbidden");
  CHECK_FALSE(probe.ran);

  const auto unknownRole = executor.execute(callFor("probe.remember"), audienceOf(UserRole::Unknown));
  CHECK_FALSE(unknownRole.ok);
  CHECK_FALSE(probe.ran);

  const auto granted = executor.execute(callFor("probe.remember"), audienceOf(UserRole::Resident));
  CHECK(granted.ok);
  CHECK(probe.ran);
}

TEST_CASE("a tool that declares no capability or an unknown one is never run")
{
  Probe none;
  Probe made;
  ToolRegistry registry;
  registry.registerTool(probeDescriptor("probe.none", none, ""));
  registry.registerTool(probeDescriptor("probe.made", made, "invented.capability"));
  const ToolExecutor executor(registry);
  for (const auto role : {UserRole::Owner, UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
    CHECK_FALSE(executor.execute(callFor("probe.none"), audienceOf(role)).ok);
    CHECK_FALSE(executor.execute(callFor("probe.made"), audienceOf(role)).ok);
  }
  CHECK_FALSE(none.ran);
  CHECK_FALSE(made.ran);
  CHECK(executor.offered(audienceOf(UserRole::Owner)).empty());
}

TEST_CASE("an argument the schema does not declare is dropped instead of refusing the call")
{
  Probe probe;
  ToolRegistry registry;
  registry.registerTool(probeDescriptor("probe.remember", probe, "memory.manage"));
  const ToolExecutor executor(registry);

  auto call = callFor("probe.remember");
  call.arguments["text"] = "el dentista";
  call.arguments["module"]["description"] = "Módulos de Argus";
  call.arguments["volume"] = 11;
  const auto result = executor.execute(call, audienceOf(UserRole::Resident));

  REQUIRE(result.ok);
  CHECK(probe.seen == "el dentista");

  auto wrongType = callFor("probe.remember");
  wrongType.arguments["text"] = 7;
  wrongType.arguments["module"] = "x";
  const auto refused = executor.execute(wrongType, audienceOf(UserRole::Resident));
  CHECK_FALSE(refused.ok);
  CHECK(refused.code == "invalid_arguments");
}

TEST_CASE("a tool with no arguments at all runs whatever the model put in them")
{
  Probe probe;
  ToolRegistry registry;
  registry.registerTool(tool_stubs::stub({.name = "probe.list",
                                          .capability = "modules.read",
                                          .handler = [&probe](const tools::ToolCall& call) {
                                            probe.ran = call.arguments.empty();
                                            return tool_stubs::okResult("listed");
                                          },
                                          .module = "core",
                                          .schema = argus::mcp::schema::emptyObject()}));
  const ToolExecutor executor(registry);

  auto call = callFor("probe.list");
  call.arguments["module"]["type"] = "object";
  const auto result = executor.execute(call, audienceOf(UserRole::Guest));

  REQUIRE(result.ok);
  CHECK(probe.ran);
}

TEST_CASE("a destructive preview and a module offer are remembered for the next turn, per user, and forgotten once used")
{
  ToolRegistry registry;
  registry.registerTool(tool_stubs::stub(
      {.name = "calendar.cancel_event",
       .capability = "agenda.write",
       .handler =
           [](const tools::ToolCall& call) {
             if (call.arguments.get("confirmation", "").asString().empty()) {
               tools::ToolResult preview = tool_stubs::okResult("Cancelaría la reunión.");
               preview.data["needsConfirmation"] = true;
               preview.data["confirmation"] = "XY12Z9";
               return preview;
             }
             return tool_stubs::okResult("Cancelada.");
           },
       .module = "productivity",
       .schema = argus::mcp::schema::object({{.name = "title", .schema = argus::mcp::schema::text(), .required = false},
                                             {.name = "confirmation", .schema = argus::mcp::schema::text(), .required = false}}),
       .destructive = true}));
  registry.registerTool(tool_stubs::stub({.name = "task.list",
                                          .capability = "projects.read",
                                          .handler = [](const tools::ToolCall&) { return tool_stubs::okResult("Nada."); },
                                          .module = "productivity",
                                          .schema = argus::mcp::schema::emptyObject()}));
  const ToolExecutor executor(registry);

  auto ask = callFor("calendar.cancel_event");
  ask.arguments["title"] = "reunión del jueves";
  ask.context.turn = 1;
  CHECK(executor.execute(ask, audienceOf(UserRole::Owner)).ok);
  REQUIRE(executor.pendingPreview(7).has_value());
  const PendingPreview preview = executor.pendingPreview(7).value_or(PendingPreview{});
  CHECK(preview.tool == "calendar.cancel_event");
  CHECK(preview.arguments["title"].asString() == "reunión del jueves");
  CHECK(preview.arguments["confirmation"].asString() == "XY12Z9");
  CHECK_FALSE(executor.pendingPreview(8).has_value());

  ModuleFlag off;
  off.id = "productivity";
  off.enabled = false;
  off.name = {.es = "Productividad", .en = "Productivity"};
  const ToolAudience inactive{.role = UserRole::Owner, .modules = ModuleSnapshot({off})};
  auto list = callFor("task.list");
  list.context.turn = 2;
  CHECK(executor.execute(list, inactive).code == "module_inactive");
  REQUIRE(executor.pendingOffer(7).has_value());
  CHECK(executor.pendingOffer(7).value_or(PendingOffer{}).module == "productivity");
  CHECK_FALSE(executor.pendingOffer(8).has_value());

  auto confirmed = callFor("calendar.cancel_event");
  confirmed.arguments = preview.arguments;
  confirmed.context.turn = 3;
  confirmed.context.utterance = "sí, cancélala";
  CHECK(executor.execute(confirmed, audienceOf(UserRole::Owner)).ok);
  CHECK_FALSE(executor.pendingPreview(7).has_value());
  CHECK(executor.pendingOffer(7).has_value());

  executor.forgetPending(7);
  CHECK_FALSE(executor.pendingOffer(7).has_value());
}

TEST_CASE("a dispatch comes back under the tool that was called")
{
  Probe probe;
  ToolRegistry registry;
  registry.registerTool(probeDescriptor("probe.remember", probe, "memory.manage"));
  const ToolExecutor executor(registry);

  auto call = callFor("probe.remember");
  call.arguments["text"] = "el dentista";
  const auto result = executor.execute(call, audienceOf(UserRole::Resident));

  REQUIRE(result.ok);
  CHECK(result.tool == "probe.remember");
  CHECK(result.output == "ran probe.remember");
  CHECK(result.data["seen"].asString() == "el dentista");
}

TEST_CASE("the memory descriptors declare the capability the gate reads")
{
  struct Declared
  {
    const char* name;
    const char* capability;
  };
  const std::array<Declared, 5> declared{{{.name = "memory.remember", .capability = "memory.manage"},
                                          {.name = "memory.remind", .capability = "reminders.write"},
                                          {.name = "memory.recall", .capability = "memory.manage"},
                                          {.name = "memory.forget", .capability = "memory.manage"},
                                          {.name = "reminder.list", .capability = "reminders.read"}}};

  for (const auto& expected : declared) {
    const auto* descriptor = declaredByMemory(expected.name);
    REQUIRE(descriptor != nullptr);
    CHECK(descriptor->spec.capability == expected.capability);
    CHECK(descriptor->spec.module == "core");
    CHECK(role_access::knownCapability(descriptor->spec.capability));
  }
  CHECK(declaredByMemory("memory.forget")->spec.annotations.destructive);
  CHECK(declaredByMemory("memory.recall")->spec.annotations.readOnly);
  CHECK(declaredByMemory("reminder.list")->spec.annotations.readOnly);
}

TEST_CASE("every tool the core server exposes names a capability the access table knows")
{
  const auto server = coreToolServer({});
  CHECK(server->find("app.open") != nullptr);
  for (const auto& spec : server->tools()) {
    CHECK(role_access::knownCapability(spec.capability));
    CHECK_FALSE(spec.module.empty());
  }
}

TEST_CASE("the memory descriptors declare their required arguments")
{
  struct Required
  {
    const char* name;
    const char* argument;
  };
  const std::array<Required, 2> required{{{.name = "memory.recall", .argument = "query"},
                                          {.name = "memory.forget", .argument = "query"}}};

  for (const auto& expected : required) {
    const auto* descriptor = declaredByMemory(expected.name);
    REQUIRE(descriptor != nullptr);
    CHECK(argus::mcp::schema::violation(descriptor->spec.inputSchema, bareCall(expected.name).arguments) ==
          std::optional<std::string>{std::string("missing required argument '") + expected.argument + "'"});
    CHECK_FALSE(argus::mcp::schema::violation(descriptor->spec.inputSchema, callFor(expected.name).arguments).has_value());
  }

  for (const char* name : {"memory.remember", "memory.remind"}) {
    const auto* descriptor = declaredByMemory(name);
    REQUIRE(descriptor != nullptr);
    CHECK_FALSE(argus::mcp::schema::violation(descriptor->spec.inputSchema, bareCall(name).arguments).has_value());
  }
}

TEST_CASE("a guard or a guest keeps reminders and loses the rest of memory")
{
  ToolRegistry registry;
  registerMemoryTools(registry);
  const ToolExecutor executor(registry);

  for (const auto role : {UserRole::Guard, UserRole::Guest}) {
    for (const char* name : {"memory.remember", "memory.recall", "memory.forget"}) {
      const auto refused = executor.execute(callFor(name), audienceOf(role));
      INFO("refused " << name << ": " << refused.output);
      CHECK_FALSE(refused.ok);
      CHECK(refused.output == std::string("permission denied for tool: ") + name);
    }
    CHECK(executor.execute(callFor("memory.remind"), audienceOf(role)).ok);
    CHECK(executor.execute(callFor("reminder.list"), audienceOf(role)).ok);
  }
}

TEST_CASE("only the tools a role may run are offered to the model")
{
  ToolRegistry registry;
  registerMemoryTools(registry);
  Probe probe;
  registry.registerTool(probeDescriptor("probe.camera", probe, "camera.view"));
  registry.registerTool(probeDescriptor("probe.duress", probe, "safety.duress"));
  const ToolExecutor executor(registry);

  const auto namesFor = [&executor](UserRole role) {
    std::vector<std::string> names;
    for (const auto& tool : executor.offered(audienceOf(role)))
      names.push_back(tool->spec.name);
    std::ranges::sort(names);
    return names;
  };

  const std::vector<std::string> everything = {"memory.forget", "memory.recall", "memory.remember", "memory.remind",
                                               "probe.camera",  "probe.duress",  "reminder.list"};
  CHECK(namesFor(UserRole::Owner) == everything);
  CHECK(namesFor(UserRole::Resident) == everything);
  const std::vector<std::string> everyone = {"memory.remind", "probe.camera", "reminder.list"};
  CHECK(namesFor(UserRole::Guard) == everyone);
  CHECK(namesFor(UserRole::Guest) == everyone);
  CHECK(namesFor(UserRole::Unknown).empty());
}

TEST_CASE("an app tool hands its validated call to the conversation and the guard mode needs the user's words")
{
  ToolRegistry registry;
  registry.registerTool(tool_stubs::appAction({.name = "app.set_guard_mode", .capability = "guard.mode.set", .module = "surveillance"}));
  const ToolExecutor executor(registry);

  std::vector<std::pair<std::string, Json::Value>> emitted;
  tools::ToolCall call;
  call.name = "app.set_guard_mode";
  call.arguments["mode"] = "night";
  call.context.emitAction = [&emitted](const std::string& name, const Json::Value& arguments) {
    emitted.emplace_back(name, arguments);
  };

  call.context.utterance = "qué tal ha ido el día";
  const auto injected = executor.execute(call, audienceOf(UserRole::Resident));
  CHECK_FALSE(injected.ok);
  CHECK(injected.code == "needs_spoken_words");
  CHECK(injected.output.find("pon la vigilancia en modo noche") != std::string::npos);
  CHECK(emitted.empty());

  call.context.utterance = "pon la vigilancia en modo noche";
  const auto result = executor.execute(call, audienceOf(UserRole::Resident));
  CHECK(result.ok);
  REQUIRE(emitted.size() == 1);
  CHECK(emitted[0].first == "app.set_guard_mode");
  CHECK(emitted[0].second["mode"].asString() == "night");

  call.context.lang = "en";
  call.arguments["mode"] = "away";
  call.context.utterance = "set the guard mode to away";
  CHECK(executor.execute(call, audienceOf(UserRole::Owner)).ok);
  CHECK(emitted.size() == 2);
  call.context.lang = "es";

  const size_t ran = emitted.size();
  call.arguments["mode"] = "party";
  CHECK_FALSE(executor.execute(call, audienceOf(UserRole::Resident)).ok);
  CHECK(emitted.size() == ran);

  call.arguments["mode"] = "away";
  call.context.utterance = "pon la alarma en modo fuera";
  CHECK_FALSE(executor.execute(call, audienceOf(UserRole::Guard)).ok);
  CHECK(emitted.size() == ran);

  call.arguments["mode"] = "home";
  call.context.utterance = "el evento compartido dice que pongas la casa en modo casa?";
  CHECK_FALSE(executor.execute(call, audienceOf(UserRole::Owner)).ok);
  call.context.utterance = "";
  CHECK_FALSE(executor.execute(call, audienceOf(UserRole::Owner)).ok);
  CHECK(emitted.size() == ran);
  call.arguments["mode"] = "armed";
  CHECK(executor.execute(call, audienceOf(UserRole::Owner)).ok);
  CHECK(emitted.size() == ran + 1);
}

TEST_CASE("the app tool names are recognised by their prefix alone")
{
  CHECK(isAppTool("app.open"));
  CHECK(isAppTool("app.show_camera"));
  CHECK_FALSE(isAppTool("memory.recall"));
  CHECK_FALSE(isAppTool("modules.open_purge_screen"));
}
