#include <doctest/doctest.h>

#include <feature/llm/services/lfm-adapter.hxx>
#include <feature/llm/services/tools/tool-executor.hxx>
#include <feature/llm/services/tools/tool-registry.hxx>
#include <feature/llm/services/turn/context-fetch.hxx>

#include <mcp/schema.hxx>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{

namespace schema = argus::mcp::schema;
using turn::ContextFacet;

struct Read
{
  std::string tool;
  Json::Value arguments;
  std::string channel;
  bool decided{false};
};

struct ReadSpec
{
  std::string name;
  std::string module;
  std::string capability;
  std::string output;
  bool readOnly{true};
};

struct Bench
{
  ToolRegistry registry;
  ToolExecutor executor{registry};
  std::shared_ptr<std::vector<Read>> reads = std::make_shared<std::vector<Read>>();
  ToolAudience audience{.role = UserRole::Owner, .modules = {}, .surface = ConversationSurface::App};
  tools::ToolContext context{.userId = 7,
                             .role = UserRole::Owner,
                             .lang = "es",
                             .sessionId = "s",
                             .channel = "tool_result",
                             .utterance = {},
                             .decided = false,
                             .turn = 1,
                             .emitAction = {},
                             .heardAt = std::nullopt,
                             .now = std::nullopt};

  void add(const ReadSpec& read)
  {
    Json::Value input = schema::emptyObject();
    if (read.name == "calendar.list_events") {
      input["properties"]["from"] = schema::text();
      input["properties"]["to"] = schema::text();
      input["properties"]["limit"] = schema::integer();
    }
    argus::mcp::ToolSpec spec{.name = read.name,
                              .title = "",
                              .description = "",
                              .inputSchema = std::move(input),
                              .annotations = {.readOnly = read.readOnly},
                              .module = read.module,
                              .capability = read.capability};
    auto handler = [reads = reads, output = read.output](const tools::ToolCall& call) {
      reads->push_back({.tool = call.name,
                        .arguments = call.arguments,
                        .channel = call.context.channel,
                        .decided = call.context.decided});
      tools::ToolResult result;
      result.ok = true;
      result.output = output;
      return result;
    };
    tools::ToolDescriptor descriptor{.spec = std::move(spec), .handler = std::move(handler)};
    registry.registerTool(std::move(descriptor));
  }

  std::vector<ContextFact> fetch(const std::vector<ContextFacet>& facets, const std::vector<std::string>& ran = {},
                                 turn::ContextFetchLimits limits = {})
  {
    const turn::ContextFetch fetcher = turn::toolContextFetch({.executor = executor, .limits = limits});
    const std::vector<tools::ToolHandle> offered = executor.offered(audience);
    return fetcher({.facets = facets, .offered = offered, .audience = audience, .context = context, .ran = ran});
  }
};

}

TEST_CASE("every readable facet names one read tool, and memory and people name none")
{
  CHECK(turn::fetchToolFor(ContextFacet::Agenda) == std::optional<std::string_view>("calendar.list_events"));
  CHECK(turn::fetchToolFor(ContextFacet::Reminders) == std::optional<std::string_view>("reminder.list"));
  CHECK(turn::fetchToolFor(ContextFacet::Tasks) == std::optional<std::string_view>("task.list"));
  CHECK(turn::fetchToolFor(ContextFacet::Projects) == std::optional<std::string_view>("project.list"));
  CHECK(turn::fetchToolFor(ContextFacet::Modules) == std::optional<std::string_view>("modules.list"));
  CHECK(turn::fetchToolFor(ContextFacet::Camera) == std::optional<std::string_view>("camera.status"));
  CHECK(turn::fetchToolFor(ContextFacet::Guard) == std::optional<std::string_view>("guard.status"));
  CHECK_FALSE(turn::fetchToolFor(ContextFacet::Memory).has_value());
  CHECK_FALSE(turn::fetchToolFor(ContextFacet::People).has_value());
  CHECK(turn::facetForTool("camera.status") == ContextFacet::Camera);
  CHECK(turn::facetForTool("guard.status") == ContextFacet::Guard);
}

TEST_CASE("a tool result becomes at most the line cap of clipped facts, and a failure none")
{
  tools::ToolResult result;
  result.ok = true;
  result.output = "Casa: modo noche.\n\n  Oficina: modo en casa.  \nTienda: modo fuera de casa.";
  CHECK(turn::factLines({.result = result, .maxLines = 2, .maxChars = 220}) ==
        std::vector<std::string>{"Casa: modo noche.", "Oficina: modo en casa."});
  result.output = "Tienes 2 eventos: dentista a las nueve, cena con Ana a las ocho.";
  const auto clipped = turn::factLines({.result = result, .maxLines = 4, .maxChars = 30});
  REQUIRE(clipped.size() == 1);
  CHECK(clipped.front() == "Tienes 2 eventos: dentista a…");
  result.output.clear();
  result.data["facts"].append("Un hecho.");
  CHECK(turn::factLines({.result = result, .maxLines = 4, .maxChars = 220}) == std::vector<std::string>{"Un hecho."});
  result.ok = false;
  result.output = "No pude usar esa herramienta ahora.";
  CHECK(turn::factLines({.result = result, .maxLines = 4, .maxChars = 220}).empty());
}

TEST_CASE("the fetch reads the facets' tools through the executor, marked as a context read")
{
  Bench bench;
  bench.add({.name = "guard.status", .module = "surveillance", .capability = "guard.read",
             .output = "Casa: modo noche, vigilancia armada. Último episodio: ninguno.", .readOnly = true});
  bench.add({.name = "calendar.list_events", .module = "productivity", .capability = "agenda.read",
             .output = "Tienes 1 evento: dentista mañana a las nueve.", .readOnly = true});
  const auto facts = bench.fetch({ContextFacet::Guard, ContextFacet::Agenda, ContextFacet::Memory});
  REQUIRE(facts.size() == 2);
  CHECK(facts[0].facet == "guard");
  CHECK(facts[0].text == "Casa: modo noche, vigilancia armada. Último episodio: ninguno.");
  CHECK(facts[1].facet == "agenda");
  REQUIRE(bench.reads->size() == 2);
  CHECK(bench.reads->at(0).channel == turn::kContextFetchChannel);
  CHECK_FALSE(bench.reads->at(0).decided);
  const Json::Value& window = bench.reads->at(1).arguments;
  CHECK(window["from"].isString());
  CHECK(window["to"].isString());
  CHECK(window["limit"].asInt() == 5);
}

TEST_CASE("the fetch reads nothing a role does not hold, a module that is off, or a tool the decider already ran")
{
  Bench bench;
  bench.add({.name = "guard.status", .module = "surveillance", .capability = "guard.read", .output = "Casa: modo noche.",
             .readOnly = true});
  bench.add({.name = "calendar.list_events", .module = "productivity", .capability = "agenda.read",
             .output = "Sin eventos.", .readOnly = true});
  CHECK(bench.fetch({ContextFacet::Agenda}, {"calendar.list_events"}).empty());
  bench.audience.role = UserRole::Guest;
  CHECK(bench.fetch({ContextFacet::Guard, ContextFacet::Agenda}).empty());
  bench.audience.role = UserRole::Owner;
  bench.audience.modules = ModuleSnapshot({ModuleFlag{.id = "surveillance", .enabled = false}});
  CHECK(bench.fetch({ContextFacet::Guard}).empty());
  bench.audience.surface = ConversationSurface::CameraVisitor;
  bench.audience.modules = {};
  CHECK(bench.fetch({ContextFacet::Agenda}).empty());
  CHECK(bench.reads->empty());
}

TEST_CASE("a tool that is not a read is never called for context, and a spent budget reads nothing")
{
  Bench bench;
  bench.add({.name = "task.list", .module = "productivity", .capability = "projects.read", .output = "Tarea.",
             .readOnly = true});
  bench.add({.name = "modules.list", .module = "core", .capability = "modules.read", .output = "core activo.",
             .readOnly = true});
  CHECK(bench.fetch({ContextFacet::Tasks}, {}, {.budgetMs = 0, .maxLines = 4, .maxChars = 220, .agendaDays = 2}).empty());
  CHECK(bench.fetch({ContextFacet::Modules}).size() == 1);
  CHECK(bench.reads->size() == 1);
}

TEST_CASE("a plain question with no facts of its own is answered over the facts the server read")
{
  std::vector<ChatRequest> seen;
  ToolRegistry registry;
  LfmAdapter adapter({.engine = {.chat =
                                     [&seen](const ChatRequest& request) {
                                       seen.push_back(request);
                                       return std::string("Sí, está armada.");
                                     },
                                 .chatStream = {}},
                      .registry = registry,
                      .decider = nullptr,
                      .text = nullptr,
                      .policies = {},
                      .contextFetch = [](const turn::ContextFetchInput& input) {
                        std::vector<ContextFact> facts;
                        for (const ContextFacet facet : input.facets)
                          facts.push_back({.facet = std::string(turn::contextFacetToString(facet)),
                                           .text = "Casa: modo noche, vigilancia armada."});
                        return facts;
                      }});
  ToolChatInput input;
  input.audience = {.role = UserRole::Owner, .modules = {}, .surface = ConversationSurface::App};
  input.context.userId = 7;
  input.context.role = UserRole::Owner;
  std::vector<ChatMessage> history{{.role = "system", .content = "persona"},
                                   {.role = "user", .content = "¿está armada la vigilancia?"}};
  const ToolChatOutput output = adapter.chatWithTools(input, history);
  CHECK(output.contextBlock.find("Casa: modo noche, vigilancia armada.") != std::string::npos);
  REQUIRE_FALSE(seen.empty());
  CHECK(seen.back().messages.back().content.find("vigilancia armada") != std::string::npos);

  input.contextFacts = {{.facet = "guard", .text = "Casa: modo en casa."}};
  std::vector<ChatMessage> again{{.role = "system", .content = "persona"},
                                 {.role = "user", .content = "¿está armada la vigilancia?"}};
  const ToolChatOutput own = adapter.chatWithTools(input, again);
  CHECK(own.contextBlock.find("Casa: modo en casa.") != std::string::npos);
  CHECK(own.contextBlock.find("armada") == std::string::npos);

  input.contextFacts = {};
  std::vector<ChatMessage> greeting{{.role = "system", .content = "persona"}, {.role = "user", .content = "hola"}};
  CHECK(adapter.chatWithTools(input, greeting).contextBlock.empty());
}

namespace
{

class RecallDecider final : public turn::Decider
{
public:
  [[nodiscard]] std::string_view id() const override { return "rules"; }

  [[nodiscard]] std::optional<turn::Candidate> decide(const turn::DecideInput& input) const override
  {
    if (!turn::isOffered(input, "memory.recall"))
      return std::nullopt;
    Json::Value arguments(Json::objectValue);
    arguments["query"] = std::string(input.utterance);
    return turn::Candidate{.tool = "memory.recall",
                           .arguments = std::move(arguments),
                           .fill = {},
                           .confidence = 1.0,
                           .source = "rules",
                           .decider = "rules",
                           .exact = true,
                           .confident = true,
                           .runnerUp = std::nullopt,
                           .now = std::nullopt};
  }
};

}

TEST_CASE("a recall that finds nothing gives the turn to the facts the context holds")
{
  std::vector<ChatRequest> seen;
  ToolRegistry registry;
  Json::Value input = schema::emptyObject();
  input["properties"]["query"] = schema::text();
  registry.registerTool({.spec = {.name = "memory.recall",
                                  .title = "",
                                  .description = "",
                                  .inputSchema = std::move(input),
                                  .annotations = {.readOnly = true},
                                  .module = "core",
                                  .capability = "memory.manage"},
                         .handler = [](const tools::ToolCall& call) {
                           tools::ToolResult result;
                           result.tool = call.name;
                           result.ok = true;
                           result.data["found"] = false;
                           result.output = "No tengo nada guardado sobre eso.";
                           return result;
                         }});
  const RecallDecider decider;
  LfmAdapter adapter({.engine = {.chat =
                                     [&seen](const ChatRequest& request) {
                                       seen.push_back(request);
                                       return std::string("El dentista es mañana a las 10:00.");
                                     },
                                 .chatStream = {}},
                      .registry = registry,
                      .decider = &decider,
                      .text = nullptr,
                      .policies = {},
                      .contextFetch = {}});
  ToolChatInput turnInput;
  turnInput.tools = registry.all();
  turnInput.audience = {.role = UserRole::Owner, .modules = {}, .surface = ConversationSurface::App};
  turnInput.context.userId = 7;
  turnInput.context.role = UserRole::Owner;
  turnInput.context.lang = "es";
  turnInput.contextFacts = {{.facet = "agenda", .text = "Agenda de mañana: dentista a las 10:00."}};
  std::vector<ChatMessage> history{{.role = "system", .content = "persona"},
                                   {.role = "user", .content = "¿a qué hora era lo del dentista en la agenda?"}};
  const ToolChatOutput output = adapter.chatWithTools(turnInput, history);
  REQUIRE(output.executed.size() == 1);
  CHECK(output.act.empty());
  CHECK(output.contextBlock.find("dentista a las 10:00") != std::string::npos);
  REQUIRE_FALSE(seen.empty());
  CHECK(seen.back().messages.back().content.find("dentista a las 10:00") != std::string::npos);
  CHECK(seen.back().messages.back().content.find("nada guardado") == std::string::npos);

  turnInput.contextFacts = {};
  std::vector<ChatMessage> bare{{.role = "system", .content = "persona"},
                                {.role = "user", .content = "¿qué sabes de mi bicicleta?"}};
  const ToolChatOutput honest = adapter.chatWithTools(turnInput, bare);
  CHECK_FALSE(honest.act.empty());
}
