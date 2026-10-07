#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <auth/module-gate.hxx>
#include <feature/llm/services/tools/core-tools.hxx>
#include <feature/llm/services/tools/tool-directory.hxx>
#include <feature/llm/services/tools/tool-executor.hxx>
#include <feature/llm/services/tools/tool-registry.hxx>
#include <mcp/local-transport.hxx>
#include <mcp/schema.hxx>
#include <mcp/server.hxx>
#include "tool-stubs.hxx"

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace
{
namespace schema = argus::mcp::schema;
using argus::mcp::ToolOutcome;

class SwitchTransport final : public argus::mcp::Transport
{
public:
  explicit SwitchTransport(std::shared_ptr<const argus::mcp::McpServer> server) : inner_(std::move(server)) {}

  [[nodiscard]] std::optional<std::string> exchange(const std::string& frame) override
  {
    if (!up.load())
      return std::nullopt;
    return inner_.exchange(frame);
  }

  std::atomic<bool> up{true};

private:
  argus::mcp::LocalTransport inner_;
};

class SwapTransport final : public argus::mcp::Transport
{
public:
  explicit SwapTransport(std::shared_ptr<const argus::mcp::McpServer> server) : server_(std::move(server)) {}

  void swap(std::shared_ptr<const argus::mcp::McpServer> server)
  {
    const std::scoped_lock lock(mutex_);
    server_ = std::move(server);
  }

  [[nodiscard]] std::optional<std::string> exchange(const std::string& frame) override
  {
    std::shared_ptr<const argus::mcp::McpServer> current;
    {
      const std::scoped_lock lock(mutex_);
      current = server_;
    }
    return current->handleBlocking(frame);
  }

private:
  std::mutex mutex_;
  std::shared_ptr<const argus::mcp::McpServer> server_;
};

std::shared_ptr<argus::mcp::McpClient> clientOver(std::shared_ptr<argus::mcp::Transport> transport)
{
  return std::make_shared<argus::mcp::McpClient>(std::move(transport), argus::mcp::ClientIdentity{.name = "test", .version = "1"});
}

argus::mcp::ToolSpec specOf(const std::string& name, const std::string& capability, const std::string& module,
                            Json::Value inputSchema = schema::emptyObject())
{
  return {.name = name,
          .title = "",
          .description = "tool " + name,
          .inputSchema = std::move(inputSchema),
          .annotations = {},
          .module = module,
          .capability = capability};
}

struct Seen
{
  argus::mcp::ToolInvocation invocation;
  int calls{0};
};

std::shared_ptr<argus::mcp::McpServer> providerServer(Seen& seen, const std::vector<std::string>& names = {"agenda.ping"})
{
  auto server = std::make_shared<argus::mcp::McpServer>(argus::mcp::ServerIdentity{.name = "provider", .version = "1", .instructions = ""});
  for (const auto& name : names) {
    server->addSync(specOf(name, "agenda.read", "productivity",
                           schema::object({{.name = "text", .schema = schema::text(), .required = false}})),
                    [&seen](const argus::mcp::ToolInvocation& invocation) {
                      seen.invocation = invocation;
                      ++seen.calls;
                      ToolOutcome outcome;
                      outcome.text = "pong " + invocation.arguments.get("text", "").asString();
                      outcome.structured["count"] = seen.calls;
                      return outcome;
                    });
  }
  return server;
}

tools::ToolCall callOf(const std::string& name)
{
  tools::ToolCall call;
  call.name = name;
  call.arguments = Json::Value(Json::objectValue);
  call.arguments["text"] = "hola";
  call.context.userId = 9;
  call.context.role = UserRole::Resident;
  call.context.lang = "es";
  call.context.sessionId = "s-9";
  call.context.utterance = "haz ping";
  call.context.decided = true;
  return call;
}

tools::ToolCall bareOf(const std::string& name)
{
  tools::ToolCall call = callOf(name);
  call.arguments = Json::Value(Json::objectValue);
  return call;
}

ToolAudience resident()
{
  return {.role = UserRole::Resident, .modules = {}};
}

ModuleSnapshot productivityOff()
{
  ModuleFlag flag{.id = "productivity", .enabled = false};
  flag.name = {.es = "Productividad", .en = "Productivity"};
  flag.intro = {.es = {.what = "Organiza tu agenda, proyectos y tareas.", .examples = {"agenda una reunión", "crea una tarea"}},
                .en = {.what = "It organizes your agenda, projects and tasks.", .examples = {"schedule a meeting", "add a task"}}};
  return ModuleSnapshot({flag});
}

class RecordingLedger final : public tools::IntentLedger
{
public:
  void offered(const tools::IntentOffer& offer) override { offers.push_back(offer); }
  void accepted(const tools::ModuleAcceptance& acceptance) override { acceptances.push_back(acceptance); }

  std::vector<tools::IntentOffer> offers;
  std::vector<tools::ModuleAcceptance> acceptances;
};
}

TEST_CASE("a provider's tools join the registry and a call reaches its handler with the caller")
{
  Seen seen;
  ToolRegistry registry;
  registry.addProvider({.id = "productivity", .client = clientOver(std::make_shared<argus::mcp::LocalTransport>(providerServer(seen)))});
  CHECK(registry.unlisted() == std::vector<std::string>{"productivity"});
  CHECK(registry.find("agenda.ping") == nullptr);

  const auto outcome = registry.refresh();
  CHECK(outcome.refreshed == std::vector<std::string>{"productivity"});
  CHECK(outcome.failed.empty());
  CHECK(registry.unlisted().empty());
  const auto tool = registry.find("agenda.ping");
  REQUIRE(tool != nullptr);
  CHECK(tool->spec.module == "productivity");
  CHECK(tool->spec.capability == "agenda.read");

  const auto result = tool->handler(callOf("agenda.ping"));
  CHECK(result.ok);
  CHECK(result.output == "pong hola");
  CHECK(result.data["count"].asInt() == 1);
  CHECK(seen.invocation.caller.userId == 9);
  CHECK(seen.invocation.caller.role == "resident");
  CHECK(seen.invocation.caller.lang == "es");
  CHECK(seen.invocation.caller.sessionId == "s-9");
  CHECK(seen.invocation.caller.utterance == "haz ping");
  CHECK(seen.invocation.caller.decided);
}

TEST_CASE("an unreachable provider keeps the tools it last listed and is reported")
{
  Seen seen;
  const auto transport = std::make_shared<SwitchTransport>(providerServer(seen));
  ToolRegistry registry;
  registry.addProvider({.id = "productivity", .client = clientOver(transport)});
  REQUIRE(registry.refresh().failed.empty());

  transport->up = false;
  const auto outcome = registry.refresh();
  CHECK(outcome.failed == std::vector<std::string>{"productivity"});
  REQUIRE(registry.find("agenda.ping") != nullptr);

  const auto result = registry.find("agenda.ping")->handler(callOf("agenda.ping"));
  CHECK_FALSE(result.ok);
  CHECK(result.code == "unavailable");
  CHECK(result.output == "No pude usar esa herramienta ahora.");
  CHECK(seen.calls == 0);

  transport->up = true;
  CHECK(registry.refresh().failed.empty());
  CHECK(registry.find("agenda.ping")->handler(callOf("agenda.ping")).ok);
}

TEST_CASE("a provider that stops serving a tool takes it out of the registry on the next refresh")
{
  Seen seen;
  Seen other;
  const auto transport = std::make_shared<SwapTransport>(providerServer(seen, {"agenda.ping", "agenda.pong"}));
  ToolRegistry registry;
  registry.addProvider({.id = "productivity", .client = clientOver(transport)});
  registry.refresh();
  CHECK(registry.names() == std::vector<std::string>{"agenda.ping", "agenda.pong"});

  transport->swap(providerServer(other, {"agenda.ping"}));
  CHECK(registry.refresh().failed.empty());
  CHECK(registry.names() == std::vector<std::string>{"agenda.ping"});
  CHECK(registry.find("agenda.pong") == nullptr);
}

TEST_CASE("when two sources declare the same tool the first declaration stays")
{
  Seen seen;
  ToolRegistry registry;
  registry.registerTool(tool_stubs::stub({.name = "agenda.ping",
                                          .capability = "agenda.read",
                                          .handler = [](const tools::ToolCall&) { return tool_stubs::okResult("local"); },
                                          .module = "productivity"}));
  registry.addProvider({.id = "productivity", .client = clientOver(std::make_shared<argus::mcp::LocalTransport>(providerServer(seen)))});
  registry.refresh();
  REQUIRE(registry.find("agenda.ping") != nullptr);
  CHECK(registry.find("agenda.ping")->handler(callOf("agenda.ping")).output == "local");
  CHECK(registry.names().size() == 1);
}

TEST_CASE("a tool result carries the server's error flag, its structure and its code")
{
  auto server = std::make_shared<argus::mcp::McpServer>(argus::mcp::ServerIdentity{.name = "p", .version = "1", .instructions = ""});
  server->addSync(specOf("agenda.fail", "agenda.read", "productivity"), [](const argus::mcp::ToolInvocation&) {
    ToolOutcome outcome;
    outcome.isError = true;
    outcome.text = "No pude abrir la agenda";
    outcome.structured["code"] = "agenda_busy";
    return outcome;
  });
  ToolRegistry registry;
  registry.addProvider({.id = "productivity", .client = clientOver(std::make_shared<argus::mcp::LocalTransport>(server))});
  registry.refresh();
  const auto result = registry.find("agenda.fail")->handler(bareOf("agenda.fail"));
  CHECK_FALSE(result.ok);
  CHECK(result.output == "No pude abrir la agenda");
  CHECK(result.code == "agenda_busy");
}

TEST_CASE("an app action in a tool result is handed to the conversation, or refused when no app is connected")
{
  auto server = std::make_shared<argus::mcp::McpServer>(argus::mcp::ServerIdentity{.name = "p", .version = "1", .instructions = ""});
  server->addSync(specOf("app.show_camera", "camera.view", "surveillance"), [](const argus::mcp::ToolInvocation&) {
    ToolOutcome outcome;
    outcome.text = "La app está mostrando la cámara garaje.";
    outcome.appAction = argus::mcp::AppAction{.name = "app.show_camera", .arguments = Json::Value(Json::objectValue)};
    outcome.appAction->arguments["camera"] = "garaje";
    return outcome;
  });
  ToolRegistry registry;
  registry.addProvider({.id = "camera", .client = clientOver(std::make_shared<argus::mcp::LocalTransport>(server))});
  registry.refresh();
  const auto tool = registry.find("app.show_camera");
  REQUIRE(tool != nullptr);

  std::vector<std::pair<std::string, Json::Value>> emitted;
  auto call = bareOf("app.show_camera");
  call.context.emitAction = [&emitted](const std::string& name, const Json::Value& arguments) {
    emitted.emplace_back(name, arguments);
  };
  const auto shown = tool->handler(call);
  CHECK(shown.ok);
  CHECK(shown.output == "La app está mostrando la cámara garaje.");
  REQUIRE(emitted.size() == 1);
  CHECK(emitted[0].first == "app.show_camera");
  CHECK(emitted[0].second["camera"].asString() == "garaje");

  const auto detached = tool->handler(bareOf("app.show_camera"));
  CHECK_FALSE(detached.ok);
  CHECK(detached.code == "app_not_connected");
  CHECK(detached.output == "La app no está conectada a esta conversación.");
}

TEST_CASE("the tools of a module that is off are still offered, and calling one offers the module instead of running it")
{
  Seen seen;
  ToolRegistry registry;
  registry.addProvider({.id = "productivity", .client = clientOver(std::make_shared<argus::mcp::LocalTransport>(providerServer(seen)))});
  registry.refresh();
  ToolExecutor executor(registry);
  const auto ledger = std::make_shared<RecordingLedger>();
  executor.attachLedger(ledger);

  const ToolAudience owner{.role = UserRole::Owner, .modules = productivityOff()};
  REQUIRE(executor.offered(owner).size() == 1);

  auto call = callOf("agenda.ping");
  call.context.role = UserRole::Owner;
  call.context.turn = 4;
  const auto result = executor.execute(call, owner);
  CHECK_FALSE(result.ok);
  CHECK(result.code == "module_inactive");
  CHECK(result.data["module"].asString() == "productivity");
  CHECK(result.output.find("El módulo Productividad está apagado") != std::string::npos);
  CHECK(result.output.find("Organiza tu agenda, proyectos y tareas.") != std::string::npos);
  CHECK(result.output.find("agenda una reunión; crea una tarea") != std::string::npos);
  CHECK(result.output.find("modules.enable con module=productivity") != std::string::npos);
  CHECK(seen.calls == 0);

  REQUIRE(ledger->offers.size() == 1);
  CHECK(ledger->offers[0].userId == 9);
  CHECK(ledger->offers[0].module == "productivity");
  CHECK(ledger->offers[0].tool == "agenda.ping");
  CHECK(ledger->offers[0].arguments["text"].asString() == "hola");
  CHECK(ledger->offers[0].utterance == "haz ping");
  CHECK(ledger->offers[0].lang == "es");
  CHECK(ledger->offers[0].role == UserRole::Owner);
}

TEST_CASE("a household member who cannot enable a module is offered to ask the owner, in the user's language")
{
  Seen seen;
  ToolRegistry registry;
  registry.addProvider({.id = "productivity", .client = clientOver(std::make_shared<argus::mcp::LocalTransport>(providerServer(seen)))});
  registry.refresh();
  const ToolExecutor executor(registry);

  const ToolAudience resident{.role = UserRole::Resident, .modules = productivityOff()};
  auto call = callOf("agenda.ping");
  call.context.lang = "en";
  const auto result = executor.execute(call, resident);
  CHECK(result.code == "module_inactive");
  CHECK(result.output.find("The Productivity module is turned off") != std::string::npos);
  CHECK(result.output.find("ask the owner of the house") != std::string::npos);
  CHECK(result.output.find("modules.request with module=productivity") != std::string::npos);
  CHECK(result.output.find("modules.enable") == std::string::npos);
}

TEST_CASE("a malformed call to a module that is off is a plain argument error, not an offer")
{
  Seen seen;
  ToolRegistry registry;
  registry.addProvider({.id = "productivity", .client = clientOver(std::make_shared<argus::mcp::LocalTransport>(providerServer(seen)))});
  registry.refresh();
  ToolExecutor executor(registry);
  const auto ledger = std::make_shared<RecordingLedger>();
  executor.attachLedger(ledger);

  auto call = callOf("agenda.ping");
  call.arguments["text"] = 5;
  const auto result = executor.execute(call, {.role = UserRole::Resident, .modules = productivityOff()});
  CHECK(result.code == "invalid_arguments");
  CHECK(ledger->offers.empty());
}

TEST_CASE("a role that never held the tool is not told about the module")
{
  Seen seen;
  ToolRegistry registry;
  registry.addProvider({.id = "productivity", .client = clientOver(std::make_shared<argus::mcp::LocalTransport>(providerServer(seen)))});
  registry.refresh();
  const ToolExecutor executor(registry);
  const ToolAudience guest{.role = UserRole::Guest, .modules = productivityOff()};
  CHECK(executor.offered(guest).empty());
  const auto result = executor.execute(callOf("agenda.ping"), guest);
  CHECK(result.code == "forbidden");
}

TEST_CASE("a destructive tool previews first and runs only on a later turn with a spoken yes")
{
  int ran = 0;
  ToolRegistry registry;
  registry.registerTool(tool_stubs::stub(
      {.name = "calendar.cancel_event",
       .capability = "agenda.write",
       .handler = [&ran](const tools::ToolCall& call) {
         tools::ToolResult result = tool_stubs::okResult("preview");
         if (call.arguments.get("confirmation", "").asString().empty()) {
           result.data["needsConfirmation"] = true;
           result.output = "Cancelaría la reunión. Código: abc123";
           return result;
         }
         ++ran;
         return tool_stubs::okResult("cancelada");
       },
       .module = "productivity",
       .schema = schema::object({{.name = "title", .schema = schema::text(), .required = true},
                                 {.name = "confirmation", .schema = schema::text(), .required = false}}),
       .destructive = true}));
  const ToolExecutor executor(registry);

  auto preview = callOf("calendar.cancel_event");
  preview.arguments = Json::Value(Json::objectValue);
  preview.arguments["title"] = "reunión";
  preview.context.turn = 10;
  preview.context.utterance = "cancela la reunión de mañana";
  const auto previewed = executor.execute(preview, resident());
  CHECK(previewed.ok);
  CHECK(previewed.data["needsConfirmation"].asBool());
  CHECK(ran == 0);

  auto confirm = preview;
  confirm.arguments["confirmation"] = "abc123";
  const auto sameTurn = executor.execute(confirm, resident());
  CHECK_FALSE(sameTurn.ok);
  CHECK(sameTurn.code == "needs_spoken_yes");

  confirm.context.turn = 11;
  confirm.context.utterance = "no sé, déjalo";
  CHECK(executor.execute(confirm, resident()).code == "needs_spoken_yes");
  confirm.context.utterance = "no, mejor no";
  CHECK(executor.execute(confirm, resident()).code == "needs_spoken_yes");
  CHECK(ran == 0);

  confirm.context.utterance = "sí, cancélala";
  const auto done = executor.execute(confirm, resident());
  CHECK(done.ok);
  CHECK(done.output == "cancelada");
  CHECK(ran == 1);

  auto stranger = confirm;
  stranger.context.userId = 99;
  stranger.context.turn = 12;
  CHECK(executor.execute(stranger, resident()).code == "needs_spoken_yes");
  CHECK(ran == 1);
}

TEST_CASE("enabling or requesting a module needs the user's own ask or a yes to an offer made earlier")
{
  int enabled = 0;
  Seen seen;
  ToolRegistry registry;
  registry.registerTool(tool_stubs::stub({.name = "modules.enable",
                                          .capability = "modules.manage",
                                          .handler = [&enabled](const tools::ToolCall&) {
                                            ++enabled;
                                            return tool_stubs::okResult("Instalando Productividad");
                                          },
                                          .module = "core",
                                          .schema = schema::object({{.name = "module", .schema = schema::text(), .required = true}})}));
  registry.addProvider({.id = "productivity", .client = clientOver(std::make_shared<argus::mcp::LocalTransport>(providerServer(seen)))});
  registry.refresh();
  ToolExecutor executor(registry);
  const auto ledger = std::make_shared<RecordingLedger>();
  executor.attachLedger(ledger);
  const ToolAudience owner{.role = UserRole::Owner, .modules = productivityOff()};

  tools::ToolCall enable;
  enable.name = "modules.enable";
  enable.arguments["module"] = "productivity";
  enable.context.userId = 1;
  enable.context.role = UserRole::Owner;
  enable.context.lang = "es";
  enable.context.turn = 20;
  enable.context.utterance = "sí";
  const auto unasked = executor.execute(enable, owner);
  CHECK_FALSE(unasked.ok);
  CHECK(unasked.code == "needs_spoken_yes");
  CHECK(enabled == 0);

  auto offerTurn = callOf("agenda.ping");
  offerTurn.context.userId = 1;
  offerTurn.context.role = UserRole::Owner;
  offerTurn.context.turn = 20;
  CHECK(executor.execute(offerTurn, owner).code == "module_inactive");

  CHECK(executor.execute(enable, owner).code == "needs_spoken_yes");
  enable.context.turn = 21;
  enable.context.utterance = "sí, actívala";
  const auto accepted = executor.execute(enable, owner);
  CHECK(accepted.ok);
  CHECK(enabled == 1);
  REQUIRE(ledger->acceptances.size() == 1);
  CHECK(ledger->acceptances[0].userId == 1);
  CHECK(ledger->acceptances[0].module == "productivity");

  enable.context.turn = 30;
  enable.context.utterance = "oye, ¿y los proyectos?";
  CHECK(executor.execute(enable, owner).code == "needs_spoken_yes");
  enable.context.userId = 2;
  enable.context.turn = 31;
  enable.context.utterance = "sí";
  CHECK(executor.execute(enable, owner).code == "needs_spoken_yes");
  enable.context.utterance = "activa productividad";
  CHECK(executor.execute(enable, owner).ok);
  CHECK(enabled == 2);
}

TEST_CASE("the directory lists a provider that was down at boot as soon as it answers")
{
  Seen seen;
  const auto transport = std::make_shared<SwitchTransport>(providerServer(seen));
  transport->up = false;
  ToolRegistry registry;
  registry.addProvider({.id = "productivity", .client = clientOver(transport)});
  ToolDirectory directory(registry, {.retryFloor = std::chrono::milliseconds(5),
                                     .retryCeiling = std::chrono::milliseconds(20),
                                     .refreshEvery = std::chrono::milliseconds(5000)});
  directory.start();
  const auto waitFor = [](const auto& condition) {
    for (int attempt = 0; attempt < 400 && !condition(); ++attempt)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return condition();
  };
  CHECK(waitFor([&registry] { return !registry.unlisted().empty(); }));
  CHECK(registry.find("agenda.ping") == nullptr);
  transport->up = true;
  CHECK(waitFor([&registry] { return registry.find("agenda.ping") != nullptr; }));
  directory.requestStop();
  CHECK(directory.drained());
}

TEST_CASE("a refresh request re-lists a provider whose tools changed without waiting for the timer")
{
  Seen seen;
  Seen other;
  ToolRegistry registry;
  const auto transport = std::make_shared<SwapTransport>(providerServer(seen, {"agenda.ping"}));
  registry.addProvider({.id = "productivity", .client = clientOver(transport)});
  ToolDirectory directory(registry, {.retryFloor = std::chrono::milliseconds(5),
                                     .retryCeiling = std::chrono::milliseconds(20),
                                     .refreshEvery = std::chrono::hours(1)});
  directory.start();
  const auto waitFor = [](const auto& condition) {
    for (int attempt = 0; attempt < 400 && !condition(); ++attempt)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    return condition();
  };
  REQUIRE(waitFor([&registry] { return registry.find("agenda.ping") != nullptr; }));
  transport->swap(providerServer(other, {"agenda.pong"}));
  directory.requestRefresh();
  CHECK(waitFor([&registry] { return registry.find("agenda.pong") != nullptr; }));
  CHECK(registry.find("agenda.ping") == nullptr);
  directory.requestStop();
  CHECK(directory.drained());
}

TEST_CASE("a tool of the core server acts for the caller the conversation declared, whatever its arguments name")
{
  int64_t actedFor = 0;
  UserRole withRole = UserRole::Unknown;
  Json::Value seenArguments;
  auto remind = tool_stubs::stub({.name = "memory.remind",
                                  .capability = "reminders.write",
                                  .handler = [&](const tools::ToolCall& call) {
                                    actedFor = call.context.userId;
                                    withRole = call.context.role;
                                    seenArguments = call.arguments;
                                    return tool_stubs::okResult("saved");
                                  }});
  ToolRegistry registry;
  registry.addProvider({.id = "llm", .client = clientOver(std::make_shared<argus::mcp::LocalTransport>(coreToolServer({std::move(remind)})))});
  registry.refresh();
  const ToolExecutor executor(registry);

  tools::ToolCall call;
  call.name = "memory.remind";
  call.arguments["text"] = "pagar la luz";
  call.arguments["userId"] = 9;
  call.arguments["user_id"] = 9;
  call.arguments["target_user_id"] = 9;
  call.context.userId = 7;
  call.context.lang = "es";
  const auto result = executor.execute(call, {.role = UserRole::Guest, .modules = {}});
  CHECK(result.ok);
  CHECK(actedFor == 7);
  CHECK(withRole == UserRole::Guest);
  CHECK(seenArguments["text"].asString() == "pagar la luz");

  call.context.userId = 11;
  CHECK(executor.execute(call, {.role = UserRole::Owner, .modules = {}}).ok);
  CHECK(actedFor == 11);
  CHECK(withRole == UserRole::Owner);
}

TEST_CASE("the real app.open opens the notifications panel for every role even when the modules are off, and still offers a module's own screen")
{
  const auto spec = appOpenSpec();
  CHECK(spec.module == "core");
  CHECK(spec.capability == "notifications.read");
  CHECK(spec.annotations.readOnly);
  Json::Value arguments(Json::objectValue);
  arguments["screen"] = "notifications";
  CHECK_FALSE(argus::mcp::schema::violation(spec.inputSchema, arguments).has_value());

  ToolRegistry registry;
  registry.addProvider({.id = "llm", .client = clientOver(std::make_shared<argus::mcp::LocalTransport>(coreToolServer({})))});
  registry.refresh();
  const ToolExecutor executor(registry);
  ModuleFlag productivity;
  productivity.id = "productivity";
  productivity.enabled = false;
  ModuleFlag surveillance;
  surveillance.id = "surveillance";
  surveillance.enabled = false;
  const ModuleSnapshot off({productivity, surveillance});
  static_cast<void>(moduleGate().apply({productivity, surveillance}));
  struct GateReset
  {
    ~GateReset() { static_cast<void>(moduleGate().apply({})); }
  } const reset;

  for (const UserRole role : {UserRole::Owner, UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
    std::vector<std::pair<std::string, Json::Value>> actions;
    tools::ToolCall call;
    call.name = "app.open";
    call.arguments["screen"] = "notifications";
    call.context.userId = 7;
    call.context.lang = "es";
    call.context.emitAction = [&actions](const std::string& name, const Json::Value& payload) { actions.emplace_back(name, payload); };
    const auto result = executor.execute(call, {.role = role, .modules = off});
    CHECK(result.ok);
    CHECK(result.output == "La app abrió las notificaciones.");
    REQUIRE(actions.size() == 1);
    CHECK(actions.front().first == "app.open");
    CHECK(actions.front().second["screen"].asString() == "notifications");
    CHECK_FALSE(actions.front().second.isMember("module"));
  }

  tools::ToolCall english;
  english.name = "app.open";
  english.arguments["screen"] = "notifications";
  english.context.userId = 7;
  english.context.lang = "en";
  english.context.emitAction = [](const std::string&, const Json::Value&) {};
  CHECK(executor.execute(english, {.role = UserRole::Guest, .modules = off}).output == "The app opened the notifications.");

  tools::ToolCall agenda = english;
  agenda.arguments["screen"] = "agenda";
  CHECK(executor.execute(agenda, {.role = UserRole::Owner, .modules = off}).code == "module_inactive");
}
