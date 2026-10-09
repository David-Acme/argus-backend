#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <auth/tool-gate.hxx>
#include <feature/llm/services/lfm-adapter.hxx>
#include <feature/llm/services/tools/tool-executor.hxx>
#include <feature/llm/services/tools/tool-registry.hxx>
#include <feature/llm/services/turn/bundle-loader.hxx>
#include <feature/llm/services/turn/context-selector.hxx>
#include <feature/llm/services/turn/gliner-extractor.hxx>
#include <feature/llm/services/turn/laya-decider.hxx>
#include <feature/llm/services/turn/onnx-session.hxx>
#include <feature/llm/services/turn/slots.hxx>
#include <mcp/local-transport.hxx>
#include <mcp/schema.hxx>
#include <mcp/server.hxx>
#include <text/iso-time.hxx>
#include <text/text-norm.hxx>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
namespace fs = std::filesystem;
namespace schema = argus::mcp::schema;

struct Pilot
{
  fs::path dir;
  std::string pin;
};

std::optional<Pilot> pilotOf(const char* dirVar, const char* pinVar)
{
  const char* dir = std::getenv(dirVar);
  const char* pin = std::getenv(pinVar);
  if (dir == nullptr || pin == nullptr || !fs::exists(fs::path(dir) / "model.onnx"))
    return std::nullopt;
  return Pilot{.dir = fs::path(dir), .pin = pin};
}

double since(std::chrono::steady_clock::time_point start)
{
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

double megabytes(std::int64_t bytes)
{
  return static_cast<double>(bytes) / (1024.0 * 1024.0);
}

Json::Value momentSchema()
{
  Json::Value property = schema::text();
  property["format"] = "date-time";
  return property;
}

struct AccessRow
{
  std::string_view tool;
  std::string_view module;
  std::string_view capability;
};

constexpr std::array<AccessRow, 21> kAccess{{
    {.tool = "memory.remember", .module = "core", .capability = "memory.manage"},
    {.tool = "memory.recall", .module = "core", .capability = "memory.manage"},
    {.tool = "memory.forget", .module = "core", .capability = "memory.manage"},
    {.tool = "memory.remind", .module = "core", .capability = "reminders.write"},
    {.tool = "reminder.list", .module = "core", .capability = "reminders.read"},
    {.tool = "calendar.create_event", .module = "productivity", .capability = "agenda.write"},
    {.tool = "calendar.list_events", .module = "productivity", .capability = "agenda.read"},
    {.tool = "calendar.cancel_event", .module = "productivity", .capability = "agenda.write"},
    {.tool = "task.create", .module = "productivity", .capability = "projects.write"},
    {.tool = "task.list", .module = "productivity", .capability = "projects.read"},
    {.tool = "task.complete", .module = "productivity", .capability = "projects.write"},
    {.tool = "project.create", .module = "productivity", .capability = "projects.write"},
    {.tool = "project.list", .module = "productivity", .capability = "projects.read"},
    {.tool = "modules.list", .module = "core", .capability = "modules.read"},
    {.tool = "modules.explain", .module = "core", .capability = "modules.read"},
    {.tool = "modules.enable", .module = "core", .capability = "modules.manage"},
    {.tool = "modules.disable", .module = "core", .capability = "modules.manage"},
    {.tool = "modules.open_purge_screen", .module = "core", .capability = "modules.manage"},
    {.tool = "app.open", .module = "core", .capability = "notifications.read"},
    {.tool = "app.show_camera", .module = "surveillance", .capability = "camera.view"},
    {.tool = "app.set_guard_mode", .module = "surveillance", .capability = "guard.mode.set"},
}};

const AccessRow* accessFor(std::string_view tool)
{
  for (const AccessRow& row : kAccess)
    if (row.tool == tool)
      return &row;
  return nullptr;
}

Json::Value schemaFor(std::string_view tool)
{
  if (tool == "calendar.create_event")
    return schema::object({{.name = "title", .schema = schema::text(), .required = true},
                           {.name = "starts_at", .schema = momentSchema(), .required = true},
                           {.name = "location", .schema = schema::text(), .required = false}});
  return schema::emptyObject();
}

std::vector<argus::mcp::ToolSpec> labelToolSpecs(std::string& error)
{
  std::vector<argus::mcp::ToolSpec> specs;
  for (const turn::LayaLabel& label : turn::layaLabels()) {
    const std::string tool(label.tool);
    const AccessRow* row = accessFor(tool);
    if (row == nullptr) {
      error = "the label " + std::string(label.label) + " serves the tool " + tool + " with no access row";
      return {};
    }
    specs.push_back({.name = tool,
                     .title = {},
                     .description = "e2e tool",
                     .inputSchema = schemaFor(tool),
                     .annotations = {},
                     .module = std::string(row->module),
                     .capability = std::string(row->capability)});
  }
  return specs;
}

struct Probe
{
  int calls{0};
  Json::Value arguments{Json::objectValue};
  argus::mcp::CallerContext caller;
};

struct World
{
  Probe probe;
  std::shared_ptr<argus::mcp::McpServer> server;
  ToolRegistry registry;
  std::vector<tools::ToolHandle> offered;
  std::string error;

  World()
  {
    server = std::make_shared<argus::mcp::McpServer>(
        argus::mcp::ServerIdentity{.name = "argus-e2e-tools", .version = "1", .instructions = {}});
    server->setGate(tool_gate::capabilities());
    for (const argus::mcp::ToolSpec& spec : labelToolSpecs(error)) {
      const bool calendar = spec.name == "calendar.create_event";
      server->addSync(spec, [this, calendar](const argus::mcp::ToolInvocation& invocation) {
        argus::mcp::ToolOutcome outcome;
        if (calendar) {
          ++probe.calls;
          probe.arguments = invocation.arguments;
          probe.caller = invocation.caller;
          outcome.text = "La reunión quedó agendada.";
        }
        else {
          outcome.text = "Hecho.";
        }
        return outcome;
      });
    }
    registry.addProvider({.id = "argus-e2e-tools",
                          .client = std::make_shared<argus::mcp::McpClient>(
                              std::make_shared<argus::mcp::LocalTransport>(server),
                              argus::mcp::ClientIdentity{.name = "e2e", .version = "1"})});
    registry.refresh();
    const ToolExecutor executor(registry);
    offered = executor.offered({.role = UserRole::Owner, .modules = {}});
  }
};

struct ScriptedEngine
{
  std::vector<ChatRequest> requests;
  std::string reply{"Listo, ya quedó en tu agenda."};

  ChatEngine engine()
  {
    return {.chat = [this](const ChatRequest& request) { return answered(request); },
            .chatStream = [this](const ChatRequest& request, TokenCallback onToken) {
              requests.push_back(request);
              onToken(reply, false);
              onToken(std::string(), true);
            }};
  }

private:
  std::string answered(const ChatRequest& request)
  {
    requests.push_back(request);
    return reply;
  }
};

struct TurnInputs
{
  const World& world;
  std::string utterance;
};

ToolChatInput inputsFor(const TurnInputs& args)
{
  return {.tools = args.world.offered,
          .audience = {.role = UserRole::Owner, .modules = {}},
          .context = {.userId = 7,
                      .role = UserRole::Owner,
                      .lang = "es",
                      .sessionId = "e2e-turn",
                      .channel = "tool_result",
                      .utterance = args.utterance,
                      .decided = false,
                      .turn = 1,
                      .emitAction = {}},
          .contextFacts = {{.facet = "agenda", .text = "Agenda de hoy: libre."},
                           {.facet = "camera", .text = "Cámaras de la casa: Cocina."}},
          .clock = "Son las 15:20 del 7 de octubre de 2026.",
          .temperature = -1.0F,
          .resetContext = false,
          .answerMaxTokens = 0,
          .prefillOnly = false};
}
}

TEST_CASE("one turn runs the whole pipeline on both pilot bundles, from the normalised utterance to the spoken result")
{
  const std::optional<Pilot> layaPilot = pilotOf("ARGUS_LAYA_BUNDLE", "ARGUS_LAYA_PIN");
  const std::optional<Pilot> glinerPilot = pilotOf("ARGUS_GLINER_BUNDLE", "ARGUS_GLINER_PIN");
  if (!layaPilot || !glinerPilot) {
    MESSAGE("no pilot bundles at ARGUS_LAYA_BUNDLE and ARGUS_GLINER_BUNDLE; the end-to-end turn did not run");
    return;
  }
  const turn::BundleLoader layaBundle({.dir = layaPilot->dir, .pin = layaPilot->pin, .kind = turn::BundleKind::Decider});
  REQUIRE(layaBundle.valid());
  const turn::BundleLoader glinerBundle({.dir = glinerPilot->dir, .pin = glinerPilot->pin, .kind = turn::BundleKind::Extractor});
  REQUIRE(glinerBundle.valid());

  const turn::BundleLoader stray({.dir = layaPilot->dir, .pin = std::string(64, '0'), .kind = turn::BundleKind::Decider});
  CHECK_FALSE(stray.valid());
  CHECK_FALSE(stray.error().empty());

  World world;
  REQUIRE(world.error.empty());
  REQUIRE_FALSE(world.offered.empty());

  turn::LayaDecider laya({.model = turn::openLayaModel({.bundle = layaBundle, .options = {}, .decode = layaBundle.decode()}),
                          .fallback = nullptr,
                          .policy = layaBundle.policy(),
                          .confidence = layaBundle.confidenceCalibration(),
                          .now = layaBundle.nowCalibration()});
  REQUIRE(laya.model_ready());
  turn::GlinerExtractor gliner({.model = turn::openGlinerModel(glinerBundle, turn::OnnxOptions{}),
                                .fallback = nullptr,
                                .thresholds = glinerBundle.thresholds()});

  static_cast<void>(laya.decide({.utterance = "hola", .lang = "es", .offered = world.offered, .modules = {}, .previousAssistant = {}}));
  static_cast<void>(gliner.extract({.tool = "calendar.create_event", .field = "title", .utterance = "hola", .lang = "es"}));

  const std::array<std::string_view, 4> candidates{
      "agéndame una reunión con Andrea el jueves a las tres",
      "pon en el calendario el dentista el martes a las diez de la mañana",
      "anota la fiesta de Rosa el sábado a las siete de la noche",
      "agéndame una reunión con Carlos mañana a las cinco de la tarde"};
  turn::Candidate chosen;
  std::string utterance;
  double layaMs = 0.0;
  for (std::string_view text : candidates) {
    const auto start = std::chrono::steady_clock::now();
    const std::optional<turn::Candidate> candidate =
        laya.decide({.utterance = text, .lang = "es", .offered = world.offered, .modules = {}, .previousAssistant = {}});
    layaMs = since(start);
    if (candidate && candidate->tool == "calendar.create_event") {
      chosen = *candidate;
      utterance = text_norm::nfc(text);
      break;
    }
  }
  REQUIRE(chosen.tool == "calendar.create_event");
  CHECK(chosen.decider == "laya");
  REQUIRE_FALSE(utterance.empty());

  turn::PolicySet policies;
  policies.set(std::string(turn::kDeciderIds[2]),
               turn::DecisionPolicy{.act = layaBundle.policy().act,
                                    .ask = layaBundle.policy().ask,
                                    .margin = layaBundle.policy().margin,
                                    .nowMin = layaBundle.policy().now});
  ScriptedEngine engine;
  LfmAdapter adapter({.engine = engine.engine(),
                      .registry = world.registry,
                      .router = nullptr,
                      .decider = &laya,
                      .text = &gliner,
                      .policies = policies});

  std::vector<ChatMessage> history;
  history.push_back({.role = "system", .content = "persona"});
  history.push_back({.role = "user", .content = utterance});
  const ToolChatInput turnInput = inputsFor({.world = world, .utterance = utterance});
  const auto turnStart = std::chrono::steady_clock::now();
  const ToolChatOutput turn = adapter.chatWithTools(turnInput, history);
  const double turnMs = since(turnStart);

  REQUIRE(turn.executed.size() == 1);
  CHECK(turn.executed.front().name == "calendar.create_event");
  CHECK(world.probe.calls == 1);
  CHECK(world.probe.caller.role == "owner");
  CHECK(world.probe.caller.userId == 7);
  CHECK(world.probe.caller.decided);
  REQUIRE(world.probe.arguments["title"].isString());
  CHECK_FALSE(world.probe.arguments["title"].asString().empty());
  const std::optional<std::int64_t> startsAt = iso_time::parse(world.probe.arguments["starts_at"].asString());
  REQUIRE(startsAt.has_value());
  CHECK(*startsAt > 0);
  CHECK(turn.contextBlock.find("Agenda de hoy") != std::string::npos);
  CHECK(turn.contextBlock.find("Cámaras de la casa") == std::string::npos);
  CHECK_FALSE(turn.reply.empty());
  CHECK(engine.requests.size() == 1);

  const tools::ToolHandle calendar = world.registry.find("calendar.create_event");
  REQUIRE(calendar != nullptr);
  tools::ToolCall guest;
  guest.name = "calendar.create_event";
  guest.arguments["title"] = "x";
  guest.arguments["starts_at"] = "2026-10-09T17:00:00+00:00";
  guest.context.userId = 9;
  guest.context.role = UserRole::Guest;
  guest.context.lang = "es";
  guest.context.utterance = "agenda algo";
  guest.context.decided = true;
  const tools::ToolResult refused = calendar->handler(guest);
  CHECK_FALSE(refused.ok);
  CHECK(refused.code == "forbidden");
  CHECK(world.probe.calls == 1);

  const auto glinerStart = std::chrono::steady_clock::now();
  const std::optional<std::string> title = gliner.extract(
      {.tool = "calendar.create_event", .field = "title", .utterance = utterance, .lang = "es"});
  const double glinerMs = since(glinerStart);
  REQUIRE(title.has_value());
  CHECK_FALSE(title->empty());

  const auto fillStart = std::chrono::steady_clock::now();
  const slots::Filled filled = slots::fill({.spec = calendar->spec,
                                            .fields = {"title", "starts_at"},
                                            .arguments = Json::Value(Json::objectValue),
                                            .context = turnInput.context,
                                            .now = *startsAt,
                                            .text = gliner,
                                            .modules = {},
                                            .answering = false});
  const double fillMs = since(fillStart);
  CHECK(filled.missing.empty());

  const turn::ContextSelector selector;
  const auto contextStart = std::chrono::steady_clock::now();
  const turn::ContextChoice choice = selector.select({.utterance = utterance, .lang = "es", .tool = "calendar.create_event"});
  const double contextMs = since(contextStart);
  CHECK_FALSE(choice.facets.empty());

  MESSAGE("e2e turn: utterance='" << utterance << "' laya_ms=" << layaMs << " gliner_ms=" << glinerMs << " fill_ms=" << fillMs
                                  << " tool_ms=" << turn.toolMs << " context_ms=" << contextMs << " speak_ms=" << turn.generateMs
                                  << " turn_ms=" << turnMs << " title='" << world.probe.arguments["title"].asString()
                                  << "' starts_at='" << world.probe.arguments["starts_at"].asString() << "' reply='" << turn.reply
                                  << "'");
  MESSAGE("e2e rss: laya_mb=" << megabytes(laya.warmRssBytes()) << " gliner_mb=" << megabytes(gliner.warmRssBytes()));
}
