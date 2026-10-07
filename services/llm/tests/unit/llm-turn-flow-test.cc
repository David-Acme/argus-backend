#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/intent/services/intent-contracts.hxx>
#include <feature/intent/services/intent-router.hxx>
#include <feature/llm/services/lfm-adapter.hxx>
#include <feature/llm/services/tools/reply-claims.hxx>
#include <feature/llm/services/tools/tool-executor.hxx>
#include <feature/llm/services/tools/tool-registry.hxx>
#include <feature/llm/services/turn/deciders.hxx>
#include <feature/llm/services/turn/model-text.hxx>
#include <feature/llm/services/turn/decision-policy.hxx>
#include <feature/llm/services/turn/slots.hxx>
#include <feature/llm/services/turn/tool-effects.hxx>
#include <feature/llm/services/turn/turn-flow.hxx>
#include <feature/llm/services/turn/turn-texts.hxx>
#include <feature/memory/services/extract/extraction-service.hxx>
#include <mcp/schema.hxx>
#include <phrase/phrase-catalog.hxx>
#include "tool-stubs.hxx"

#include <text/iso-time.hxx>
#include <text/name-match.hxx>

#include <algorithm>
#include <cstdint>
#include <ctime>
#include <deque>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace
{
namespace schema = argus::mcp::schema;

constexpr std::string_view kCode = "AB12CD";

std::string tomorrowAtFive()
{
  const std::time_t now = std::time(nullptr);
  std::tm local{};
  localtime_r(&now, &local);
  local.tm_mday += 1;
  local.tm_hour = 17;
  local.tm_min = 0;
  local.tm_sec = 0;
  local.tm_isdst = -1;
  return iso_time::format(static_cast<int64_t>(std::mktime(&local))).substr(0, 16);
}

Json::Value dateTime()
{
  Json::Value property = schema::text();
  property["format"] = "date-time";
  return property;
}

class ScriptedDecider final : public turn::Decider
{
public:
  std::optional<turn::Candidate> next;
  mutable int asked{0};
  mutable std::string heardPrevious;
  std::string name{"script"};

  [[nodiscard]] std::string_view id() const override { return name; }

  [[nodiscard]] std::optional<turn::Candidate> decide(const turn::DecideInput& input) const override
  {
    ++asked;
    heardPrevious = std::string(input.previousAssistant);
    if (next && turn::isOffered(input, next->tool))
      return next;
    return std::nullopt;
  }
};

turn::Candidate candidate(std::string tool, double confidence)
{
  return {.tool = std::move(tool),
          .arguments = Json::Value(Json::objectValue),
          .fill = {},
          .confidence = confidence,
          .source = "script",
          .decider = "script",
          .exact = true,
          .confident = true,
          .runnerUp = std::nullopt};
}

struct World
{
  ToolRegistry registry;
  ToolExecutor executor{registry};
  slots::RuleText text;
  turn::RuleDecider rules;
  ScriptedDecider scripted;
  turn::TurnFlow flow;
  std::vector<tools::ToolCall> ran;
  std::vector<tools::ToolHandle> offered;
  ToolAudience audience{.role = UserRole::Owner, .modules = {}};
  tools::ToolContext context;
  int64_t now{static_cast<int64_t>(std::time(nullptr))};
  bool createOk{true};
  std::string previous;
  std::vector<std::string> projects{"Casa"};
  bool listsProjects{true};

  explicit World(UserRole role = UserRole::Owner)
      : flow({.executor = executor, .decider = &rules, .text = &text, .policies = turn::PolicySet({.act = 0.90, .ask = 0.60, .margin = 0.10})})
  {
    audience.role = role;
    context = {.userId = 7,
               .role = role,
               .lang = "es",
               .sessionId = "voice-7-1",
               .channel = "tool_result",
               .utterance = {},
               .decided = false,
               .turn = 0,
               .emitAction = {}};
    add(tool_stubs::stub({.name = "calendar.create_event",
                          .capability = "agenda.write",
                          .handler = [this](const tools::ToolCall& call) {
                            ran.push_back(call);
                            if (!createOk) {
                              tools::ToolResult failed;
                              failed.output = "No pude agendar.";
                              return failed;
                            }
                            return tool_stubs::okResult("Agendé «" + call.arguments["title"].asString() + "» para " +
                                                        call.arguments["starts_at"].asString() + ".");
                          },
                          .module = "productivity",
                          .schema = schema::object({{.name = "title", .schema = schema::text(), .required = true},
                                                    {.name = "starts_at", .schema = dateTime(), .required = true}})}));
    add(tool_stubs::stub({.name = "calendar.cancel_event",
                          .capability = "agenda.write",
                          .handler = [this](const tools::ToolCall& call) {
                            ran.push_back(call);
                            const std::string title = call.arguments["title"].asString();
                            const std::string token = call.arguments.get("confirmation", "").asString();
                            if (token.empty()) {
                              tools::ToolResult preview = tool_stubs::okResult("Esto cancelaría «" + title + "».");
                              preview.data["needsConfirmation"] = true;
                              preview.data["confirmation"] = std::string(kCode);
                              return preview;
                            }
                            if (token != kCode) {
                              tools::ToolResult wrong;
                              wrong.output = "Ese código no vale.";
                              return wrong;
                            }
                            return tool_stubs::okResult("Cancelado: «" + title + "».");
                          },
                          .module = "productivity",
                          .schema = schema::object({{.name = "title", .schema = schema::text(), .required = true},
                                                    {.name = "confirmation", .schema = schema::text(), .required = false}}),
                          .destructive = true}));
    add(tool_stubs::stub({.name = "task.create",
                          .capability = "projects.write",
                          .handler = [this](const tools::ToolCall& call) { return createTask(call); },
                          .module = "productivity",
                          .schema = schema::object({{.name = "title", .schema = schema::text(), .required = true},
                                                    {.name = "project", .schema = schema::text(), .required = false}})}));
    add(tool_stubs::stub({.name = "project.create",
                          .capability = "projects.write",
                          .handler = [this](const tools::ToolCall& call) {
                            ran.push_back(call);
                            projects.push_back(call.arguments["name"].asString());
                            return tool_stubs::okResult("Proyecto creado: " + call.arguments["name"].asString() + ".");
                          },
                          .module = "productivity",
                          .schema = schema::object({{.name = "name", .schema = schema::text(), .required = true}})}));
    auto list = tool_stubs::stub({.name = "task.list",
                                  .capability = "projects.read",
                                  .handler = [this](const tools::ToolCall& call) {
                                    ran.push_back(call);
                                    return tool_stubs::okResult("Tienes dos tareas.");
                                  },
                                  .module = "productivity",
                                  .schema = schema::emptyObject()});
    list.spec.annotations.readOnly = true;
    add(std::move(list));
    add(tool_stubs::stub({.name = "modules.enable",
                          .capability = "modules.manage",
                          .handler = [this](const tools::ToolCall& call) {
                            ran.push_back(call);
                            return tool_stubs::okResult("Activé el módulo " + call.arguments["module"].asString() + ".");
                          },
                          .module = "core",
                          .schema = schema::object({{.name = "module", .schema = schema::text(), .required = true}})}));
    add(tool_stubs::stub({.name = "modules.request",
                          .capability = "modules.request",
                          .handler = [this](const tools::ToolCall& call) {
                            ran.push_back(call);
                            return tool_stubs::okResult("Le pedí al dueño activar " + call.arguments["module"].asString() + ".");
                          },
                          .module = "core",
                          .schema = schema::object({{.name = "module", .schema = schema::text(), .required = true}})}));
    auto recall = tool_stubs::stub({.name = "memory.recall",
                                    .capability = "memory.manage",
                                    .handler = [this](const tools::ToolCall& call) {
                                      ran.push_back(call);
                                      return tool_stubs::okResult("Me dijiste que sí.");
                                    },
                                    .module = "core",
                                    .schema = schema::object({{.name = "query", .schema = schema::text(), .required = true}})});
    recall.spec.annotations.readOnly = true;
    add(std::move(recall));
    add(tool_stubs::stub({.name = "memory.remember",
                          .capability = "memory.manage",
                          .handler = [this](const tools::ToolCall& call) {
                            ran.push_back(call);
                            return tool_stubs::okResult("Guardado.");
                          },
                          .module = "core",
                          .schema = schema::object({{.name = "text", .schema = schema::text(), .required = true}})}));
    refresh();
  }

  void add(tools::ToolDescriptor descriptor) { registry.registerTool(std::move(descriptor)); }

  tools::ToolResult createTask(const tools::ToolCall& call)
  {
    ran.push_back(call);
    const std::string title = call.arguments["title"].asString();
    const std::string asked = call.arguments.get("project", "").asString();
    const auto refuse = [&](const std::string& code, const std::string& text) {
      tools::ToolResult result;
      result.output = text;
      result.code = code;
      if (listsProjects)
        for (const std::string& name : projects)
          result.data["projects"].append(name);
      return result;
    };
    if (!asked.empty()) {
      const auto match = text_norm::matchName(projects, asked);
      if (match.kind != text_norm::NameMatchKind::Exact)
        return refuse("unknown_project", "No encuentro ese proyecto.");
      return tool_stubs::okResult("Anoté la tarea «" + title + "» en " + projects.at(match.hits.front()) + ".");
    }
    if (projects.empty())
      return refuse("no_projects", "Todavía no tienes proyectos.");
    if (projects.size() > 1)
      return refuse("project_needed", "¿En cuál proyecto va?");
    return tool_stubs::okResult("Anoté la tarea «" + title + "».");
  }

  void refresh() { offered = executor.offered(audience); }

  void productivityOff()
  {
    ModuleFlag off;
    off.id = "productivity";
    off.enabled = false;
    off.name = {.es = "Productividad", .en = "Productivity"};
    off.summary = {.es = "agenda, tareas y proyectos", .en = "agenda, tasks and projects"};
    audience.modules = ModuleSnapshot({off});
  }

  turn::Outcome say(const std::string& utterance)
  {
    ++context.turn;
    return flow.run({.utterance = utterance,
                     .offered = offered,
                     .audience = audience,
                     .context = context,
                     .now = now,
                     .previousAssistant = previous});
  }
};

turn::Candidate must(const std::optional<turn::Candidate>& value)
{
  REQUIRE(value.has_value());
  return value.value_or(turn::Candidate{});
}

std::string said(const turn::Outcome& outcome)
{
  REQUIRE(outcome.question.has_value());
  return outcome.question.value_or(std::string());
}

std::string titled(const std::optional<std::string>& title)
{
  REQUIRE(title.has_value());
  return title.value_or(std::string());
}

const turn::Finding* findingOf(const turn::Outcome& outcome, turn::FindingKind kind)
{
  for (const auto& finding : outcome.findings)
    if (finding.kind == kind)
      return &finding;
  return nullptr;
}

class SilentClassifier final : public intent::IIntentClassifier
{
public:
  [[nodiscard]] bool isLoaded() const override { return false; }
  [[nodiscard]] std::vector<intent::IntentHit> score(const std::string&) const override { return {}; }
};

class ForgetClassifier final : public intent::IIntentClassifier
{
public:
  [[nodiscard]] bool isLoaded() const override { return true; }
  [[nodiscard]] std::vector<intent::IntentHit> score(const std::string&) const override
  {
    return {{.intent = intent::ToolIntent::MemoryForget, .score = 0.97F}, {.intent = intent::ToolIntent::None, .score = 0.01F}};
  }
};

struct ScriptedEngine
{
  std::deque<std::string> replies;
  std::vector<ChatRequest> requests;
  std::size_t chunk{0};

  std::string next(const ChatRequest& request)
  {
    requests.push_back(request);
    if (replies.empty())
      return "Listo.";
    std::string reply = std::move(replies.front());
    replies.pop_front();
    return reply;
  }

  ChatEngine engine()
  {
    return {.chat = [this](const ChatRequest& request) { return next(request); },
            .chatStream =
                [this](const ChatRequest& request, const TokenCallback& onToken) {
                  const std::string reply = next(request);
                  if (chunk == 0) {
                    onToken(reply, false);
                  }
                  else {
                    for (std::size_t at = 0; at < reply.size(); at += chunk)
                      onToken(reply.substr(at, chunk), false);
                  }
                  onToken("", true);
                }};
  }
};

struct Spoken
{
  World world;
  ScriptedEngine script;
  LfmAdapter adapter;
  std::string heard;
  std::vector<std::string> pieces;

  explicit Spoken(UserRole role = UserRole::Owner)
      : world(role),
        adapter({.engine = script.engine(),
                 .registry = world.registry,
                 .router = nullptr,
                 .decider = &world.rules,
                 .text = &world.text,
                 .policies = turn::PolicySet({.act = 0.90, .ask = 0.60, .margin = 0.10})})
  {
  }

  ToolChatInput input()
  {
    ToolChatInput loop;
    loop.tools = adapter.executor().offered(world.audience);
    loop.audience = world.audience;
    loop.context = world.context;
    loop.context.turn = ++world.context.turn;
    return loop;
  }

  ToolChatOutput sync(const std::string& utterance)
  {
    std::vector<ChatMessage> history{{.role = "system", .content = "persona"}, {.role = "user", .content = utterance}};
    return adapter.chatWithTools(input(), history);
  }

  ToolChatOutput stream(const std::string& utterance)
  {
    std::vector<ChatMessage> history{{.role = "system", .content = "persona"}, {.role = "user", .content = utterance}};
    const TokenCallback onToken = [this](const std::string& token, bool) {
      if (!token.empty())
        pieces.push_back(token);
      heard += token;
    };
    return adapter.chatWithToolsStream({.input = input(), .history = history, .onToken = onToken});
  }
};
}

TEST_CASE("a confidence is judged against two thresholds and a margin, all of them data")
{
  constexpr turn::DecisionPolicy policy{.act = 0.90, .ask = 0.60, .margin = 0.10};
  CHECK(turn::judge(policy, {.confidence = 1.0, .runnerUp = std::nullopt}) == turn::Verdict::Act);
  CHECK(turn::judge(policy, {.confidence = 0.90, .runnerUp = std::nullopt}) == turn::Verdict::Act);
  CHECK(turn::judge(policy, {.confidence = 0.75, .runnerUp = std::nullopt}) == turn::Verdict::Ask);
  CHECK(turn::judge(policy, {.confidence = 0.60, .runnerUp = std::nullopt}) == turn::Verdict::Ask);
  CHECK(turn::judge(policy, {.confidence = 0.59, .runnerUp = std::nullopt}) == turn::Verdict::Pass);
  CHECK(turn::judge(policy, {.confidence = 0.95, .runnerUp = 0.70}) == turn::Verdict::Act);
  CHECK(turn::judge(policy, {.confidence = 0.95, .runnerUp = 0.90}) == turn::Verdict::Choose);
  CHECK(turn::judge(policy, {.confidence = 0.75, .runnerUp = 0.70}) == turn::Verdict::Choose);
  CHECK(turn::judge(policy, {.confidence = 0.95, .runnerUp = 0.30}) == turn::Verdict::Act);
  constexpr turn::DecisionPolicy strict{.act = 0.99, .ask = 0.99, .margin = 0.0};
  CHECK(turn::judge(strict, {.confidence = 0.98, .runnerUp = std::nullopt}) == turn::Verdict::Pass);
  constexpr turn::DecisionPolicy fallback{};
  CHECK(turn::judge(fallback, {.confidence = 0.5, .runnerUp = std::nullopt}) == turn::Verdict::Pass);
  CHECK(turn::judge(fallback, {.confidence = 0.9, .runnerUp = 0.89}) == turn::Verdict::Act);
}

TEST_CASE("each decider has its own policy and the others fall back to the default")
{
  turn::PolicySet policies({.act = 0.90, .ask = 0.60, .margin = 0.10});
  policies.set("laya", {.act = 0.97, .ask = 0.80, .margin = 0.05});
  CHECK(policies.of("laya").act == doctest::Approx(0.97));
  CHECK(policies.of("router").act == doctest::Approx(0.90));
  CHECK(policies.of("router").margin == doctest::Approx(0.10));
}

TEST_CASE("deciders stack in order and a decider that abstains hands the turn on")
{
  World world;
  ScriptedDecider first;
  ScriptedDecider second;
  second.next = candidate("task.list", 0.95);
  const turn::FirstOf stack({&first, &second});
  const turn::DecideInput input{.utterance = "x", .lang = "es", .offered = world.offered, .modules = world.audience.modules};
  const turn::Candidate decided = must(stack.decide(input));
  CHECK(decided.tool == "task.list");
  CHECK(first.asked == 1);

  first.next = candidate("task.create", 0.7);
  CHECK(must(stack.decide(input)).tool == "task.create");
  CHECK(second.asked == 1);

  first.next = candidate("task.unknown", 1.0);
  second.next.reset();
  CHECK_FALSE(stack.decide(input).has_value());
}

TEST_CASE("the rule decider proposes only a tool the turn offers")
{
  World world;
  const turn::RuleDecider rules;
  const std::string utterance = "agéndame una reunión con Andrea mañana a las 5 de la tarde";
  const turn::Candidate decided =
      must(rules.decide({.utterance = utterance, .lang = "es", .offered = world.offered, .modules = world.audience.modules}));
  CHECK(decided.tool == "calendar.create_event");
  CHECK(decided.confidence == doctest::Approx(1.0));

  const std::vector<tools::ToolHandle> memoryOnly{world.registry.find("memory.remember")};
  CHECK_FALSE(rules.decide({.utterance = utterance, .lang = "es", .offered = memoryOnly, .modules = world.audience.modules}).has_value());
}

TEST_CASE("an explicit agenda command is the agenda's, and the app grammar keeps what is the app's")
{
  World world;
  world.add(tool_stubs::appAction({.name = "app.open", .capability = "notifications.read", .module = "core"}));
  world.add(tool_stubs::appAction({.name = "app.show_camera", .capability = "camera.view", .module = "surveillance"}));
  world.refresh();
  const turn::RuleDecider rules;
  const auto toolOf = [&](const std::string& utterance) {
    const auto decided = rules.decide({.utterance = utterance, .lang = "es", .offered = world.offered, .modules = world.audience.modules});
    return decided ? decided->tool : std::string();
  };
  CHECK(toolOf("pon en mi calendario la cita con el dentista el martes a las diez de la mañana") == "calendar.create_event");
  CHECK(toolOf("a ver pon en el calendario la cita del médico el martes a las nueve") == "calendar.create_event");
  CHECK(toolOf("abre la agenda") == "app.open");
  CHECK(toolOf("muéstrame la cámara del garaje") == "app.show_camera");
  CHECK(toolOf("hola qué tal").empty());
}

TEST_CASE("the router decider names a memory tool with the words as its argument and never carries a code")
{
  World world;
  PhraseCatalog catalog;
  catalog.build();
  const SilentClassifier silent;
  const IntentRouter byRules({.catalog = catalog, .model = silent, .recurrent = {}});
  const turn::RouterDecider remembers(&byRules);
  const std::string statement = "recuerda que mi hermana viene los domingos";
  const turn::Candidate saved =
      must(remembers.decide({.utterance = statement, .lang = "es", .offered = world.offered, .modules = world.audience.modules}));
  CHECK(saved.tool == "memory.remember");
  CHECK(saved.arguments["text"].asString() == statement);
  CHECK(saved.confidence == doctest::Approx(1.0));

  const ForgetClassifier forgets;
  const IntentRouter byModel({.catalog = catalog, .model = forgets, .recurrent = {}});
  const turn::RouterDecider forgetter(&byModel);
  ToolRegistry registry;
  registry.registerTool(tool_stubs::stub({.name = "memory.forget",
                                          .capability = "memory.manage",
                                          .handler = [](const tools::ToolCall&) { return tool_stubs::okResult("Olvidado."); },
                                          .module = "core",
                                          .schema = schema::object({{.name = "query", .schema = schema::text(), .required = true},
                                                                    {.name = "confirmation", .schema = schema::text(), .required = false}}),
                                          .destructive = true}));
  const std::vector<tools::ToolHandle> offered{registry.find("memory.forget")};
  const turn::Candidate forget =
      must(forgetter.decide({.utterance = "olvida lo del dentista", .lang = "es", .offered = offered, .modules = world.audience.modules}));
  CHECK(forget.tool == "memory.forget");
  CHECK_FALSE(forget.arguments.isMember("confirmation"));
  CHECK(forget.confidence >= 0.90);

  CHECK_FALSE(turn::RouterDecider(nullptr).decide({.utterance = statement, .lang = "es", .offered = world.offered, .modules = world.audience.modules}).has_value());
}

TEST_CASE("a request with its title and time is decided, filled and run, and what is said carries what was stored")
{
  World world;
  const auto outcome = world.say("agéndame una reunión con Andrea mañana a las 5 de la tarde");
  REQUIRE(world.ran.size() == 1);
  const Json::Value& arguments = world.ran.front().arguments;
  CHECK(arguments["title"].asString() == "Reunión con Andrea");
  const std::string startsAt = arguments["starts_at"].asString();
  CHECK(startsAt.starts_with(tomorrowAtFive()));
  CHECK(world.ran.front().context.decided);
  CHECK(world.ran.front().context.utterance == "agéndame una reunión con Andrea mañana a las 5 de la tarde");
  REQUIRE(outcome.steps.size() == 1);
  CHECK(outcome.wrote);
  CHECK_FALSE(outcome.question.has_value());
  const auto* done = findingOf(outcome, turn::FindingKind::Done);
  REQUIRE(done != nullptr);
  CHECK(done->text == "Agendé «Reunión con Andrea» para " + startsAt + ".");
  const std::string notes = turn::TurnFlow::notes(outcome, "es");
  CHECK(notes.find("Agendé «Reunión con Andrea» para " + startsAt) != std::string::npos);
  CHECK(notes.find("List of tools") == std::string::npos);
}

TEST_CASE("a missing time becomes a question and the answer completes the call")
{
  World world;
  const auto first = world.say("agéndame una reunión con Andrea");
  CHECK(world.ran.empty());
  CHECK(said(first) == turn_texts::slotQuestion({.tool = "calendar.create_event", .slot = "starts_at", .lang = "es"}));
  CHECK_FALSE(first.wrote);

  const auto second = world.say("mañana a las 5 de la tarde");
  REQUIRE(world.ran.size() == 1);
  CHECK(world.ran.front().arguments["title"].asString() == "Reunión con Andrea");
  CHECK(world.ran.front().arguments["starts_at"].asString().starts_with(tomorrowAtFive()));
  CHECK(second.wrote);
  CHECK_FALSE(second.question.has_value());
}

TEST_CASE("a missing title is asked for and the answer becomes the title")
{
  World world;
  const auto first = world.say("anota una tarea");
  CHECK(world.ran.empty());
  CHECK(said(first) == turn_texts::slotQuestion({.tool = "task.create", .slot = "title", .lang = "es"}));

  const auto second = world.say("llamar al dentista");
  REQUIRE(world.ran.size() == 1);
  CHECK(world.ran.front().arguments["title"].asString() == "Llamar al dentista");
  CHECK(second.wrote);
}

TEST_CASE("an answer that is not an answer is asked for again once and then given up on")
{
  World world;
  REQUIRE(world.say("anota una tarea").question.has_value());
  const auto again = world.say("...");
  CHECK(said(again) == turn_texts::slotQuestion({.tool = "task.create", .slot = "title", .lang = "es"}));
  const auto gaveUp = world.say("...");
  CHECK(said(gaveUp) == turn_texts::misunderstood("es"));
  CHECK(world.ran.empty());
  CHECK_FALSE(world.say("...").question.has_value());
}

TEST_CASE("a new command while a question is open replaces the question")
{
  World world;
  REQUIRE(world.say("anota una tarea").question.has_value());
  const auto fresh = world.say("agéndame una reunión con Andrea mañana a las 5 de la tarde");
  CHECK_FALSE(fresh.question.has_value());
  REQUIRE(world.ran.size() == 1);
  CHECK(world.ran.front().name == "calendar.create_event");
  CHECK(world.say("llamar al dentista").steps.empty());
}

TEST_CASE("a confidence between the two thresholds is asked about, and yes runs it")
{
  World world;
  world.flow.useDecider(world.scripted);
  turn::Candidate maybe = candidate("memory.remember", 0.70);
  maybe.arguments["text"] = "mi hermana viene los domingos";
  world.scripted.next = maybe;

  const auto asked = world.say("mi hermana viene los domingos");
  CHECK(world.ran.empty());
  CHECK(said(asked) == turn_texts::confirmQuestion(
                           {.tool = "memory.remember", .lang = "es", .details = {.title = "mi hermana viene los domingos", .when = {}, .module = {}}}));

  world.scripted.next.reset();
  const auto yes = world.say("sí");
  REQUIRE(world.ran.size() == 1);
  CHECK(world.ran.front().arguments["text"].asString() == "mi hermana viene los domingos");
  CHECK(yes.wrote);
}

TEST_CASE("no to a question runs nothing, is said so, and the question is forgotten")
{
  World world;
  world.flow.useDecider(world.scripted);
  world.scripted.next = candidate("task.list", 0.70);
  REQUIRE(world.say("hay tareas").question.has_value());
  world.scripted.next.reset();
  const auto no = world.say("no, gracias");
  CHECK(world.ran.empty());
  CHECK(findingOf(no, turn::FindingKind::Declined) != nullptr);
  CHECK(turn::TurnFlow::notes(no, "es") == turn_texts::declined("es"));
  const auto later = world.say("sí");
  CHECK(later.steps.empty());
  CHECK(world.ran.empty());
}

TEST_CASE("another sentence instead of an answer drops the question and is decided on its own")
{
  World world;
  world.flow.useDecider(world.scripted);
  world.scripted.next = candidate("task.list", 0.70);
  REQUIRE(world.say("hay tareas").question.has_value());
  world.scripted.next = candidate("task.list", 1.0);
  const auto fresh = world.say("más bien dime qué tareas tengo");
  REQUIRE(world.ran.size() == 1);
  CHECK(fresh.wrote == false);
  CHECK(fresh.steps.size() == 1);
}

TEST_CASE("a confidence below the lower threshold is no tool, and only an asked action says so")
{
  World world;
  world.flow.useDecider(world.scripted);
  world.scripted.next = candidate("task.list", 0.30);
  const auto plain = world.say("hola qué tal");
  CHECK(plain.steps.empty());
  CHECK(plain.findings.empty());
  CHECK_FALSE(plain.question.has_value());

  const auto asked = world.say("agéndame una reunión con Andrea mañana a las 5 de la tarde");
  CHECK(asked.steps.empty());
  CHECK(world.ran.empty());
  REQUIRE(findingOf(asked, turn::FindingKind::Unactionable) != nullptr);
  CHECK(turn::TurnFlow::notes(asked, "es") == turn_texts::unactionable("es"));
}

TEST_CASE("a destructive call is previewed, the code never reaches the notes, and yes confirms with the stored code")
{
  World world;
  world.flow.useDecider(world.scripted);
  turn::Candidate cancel = candidate("calendar.cancel_event", 1.0);
  cancel.arguments["title"] = "Cena con Marta";
  world.scripted.next = cancel;

  const auto preview = world.say("cancela la cena con Marta");
  REQUIRE(preview.steps.size() == 1);
  CHECK_FALSE(preview.wrote);
  const auto* pending = findingOf(preview, turn::FindingKind::Preview);
  REQUIRE(pending != nullptr);
  CHECK(pending->text == "Esto cancelaría «Cena con Marta».");
  const std::string notes = turn::TurnFlow::notes(preview, "es");
  CHECK(notes.find(std::string(kCode)) == std::string::npos);
  CHECK(notes.find("Esto cancelaría «Cena con Marta».") != std::string::npos);

  world.scripted.next.reset();
  const auto confirmed = world.say("sí, cancélala");
  REQUIRE(world.ran.size() == 2);
  CHECK(world.ran.back().arguments["confirmation"].asString() == kCode);
  CHECK(world.ran.back().arguments["title"].asString() == "Cena con Marta");
  CHECK(confirmed.wrote);
  const auto* done = findingOf(confirmed, turn::FindingKind::Done);
  REQUIRE(done != nullptr);
  CHECK(done->text == "Cancelado: «Cena con Marta».");

  const auto again = world.say("sí");
  CHECK(again.steps.empty());
  CHECK(world.ran.size() == 2);
}

TEST_CASE("a no that brings a new command is the new command and not a refusal")
{
  World world;
  world.flow.useDecider(world.scripted);
  turn::Candidate cancel = candidate("calendar.cancel_event", 1.0);
  cancel.arguments["title"] = "Cena con Marta";
  world.scripted.next = cancel;
  REQUIRE(world.say("cancela la cena con Marta").steps.size() == 1);

  world.flow.useDecider(world.rules);
  const auto fresh = world.say("no, mejor agéndame una reunión con Andrea mañana a las 5 de la tarde");
  CHECK(findingOf(fresh, turn::FindingKind::Declined) == nullptr);
  REQUIRE(world.ran.size() == 2);
  CHECK(world.ran.back().name == "calendar.create_event");
  CHECK(world.say("sí").steps.empty());
}

TEST_CASE("no to a preview cancels it, and a later yes does not resurrect it")
{
  World world;
  world.flow.useDecider(world.scripted);
  turn::Candidate cancel = candidate("calendar.cancel_event", 1.0);
  cancel.arguments["title"] = "Cena con Marta";
  world.scripted.next = cancel;
  REQUIRE(world.say("cancela la cena con Marta").steps.size() == 1);
  world.scripted.next.reset();
  const auto no = world.say("no, déjala");
  CHECK(findingOf(no, turn::FindingKind::Declined) != nullptr);
  CHECK(world.say("sí").steps.empty());
  CHECK(world.ran.size() == 1);
}

TEST_CASE("a module that is off is offered, and yes turns it on for the owner and asks the owner for anyone else")
{
  World owner;
  owner.productivityOff();
  owner.flow.useDecider(owner.scripted);
  owner.scripted.next = candidate("task.list", 1.0);
  const auto offer = owner.say("qué tareas tengo");
  REQUIRE(offer.steps.size() == 1);
  CHECK(offer.steps.front().result.code == "module_inactive");
  CHECK_FALSE(offer.wrote);
  const auto* offered = findingOf(offer, turn::FindingKind::Offer);
  REQUIRE(offered != nullptr);
  CHECK(offered->text.find("Productividad") != std::string::npos);
  CHECK(offered->text.find("modules.enable") == std::string::npos);
  CHECK(offered->text.find("llama a") == std::string::npos);
  CHECK(owner.ran.empty());

  owner.scripted.next.reset();
  const auto yes = owner.say("sí, actívalo");
  REQUIRE(owner.ran.size() == 1);
  CHECK(owner.ran.front().name == "modules.enable");
  CHECK(owner.ran.front().arguments["module"].asString() == "productivity");
  CHECK(yes.wrote);

  World resident(UserRole::Resident);
  resident.productivityOff();
  resident.flow.useDecider(resident.scripted);
  resident.scripted.next = candidate("task.list", 1.0);
  REQUIRE(resident.say("qué tareas tengo").steps.size() == 1);
  resident.scripted.next.reset();
  resident.say("sí");
  REQUIRE(resident.ran.size() == 1);
  CHECK(resident.ran.front().name == "modules.request");
  CHECK(resident.ran.front().arguments["module"].asString() == "productivity");
}

TEST_CASE("saying no to an offer drops it, and a yes that was not asked for does nothing")
{
  World world;
  world.productivityOff();
  world.flow.useDecider(world.scripted);
  world.scripted.next = candidate("task.list", 1.0);
  REQUIRE(world.say("qué tareas tengo").steps.size() == 1);
  world.scripted.next.reset();
  CHECK(findingOf(world.say("no, así está bien"), turn::FindingKind::Declined) != nullptr);
  CHECK(world.say("sí").steps.empty());
  CHECK(world.ran.empty());

  World stranger;
  CHECK(stranger.say("sí").steps.empty());
}

TEST_CASE("a tool the role does not hold is never decided, so the owner-only tools are not reached by a resident")
{
  World resident(UserRole::Resident);
  const auto names = [&resident] {
    std::vector<std::string> out;
    out.reserve(resident.offered.size());
    for (const auto& handle : resident.offered)
      out.push_back(handle->spec.name);
    return out;
  }();
  CHECK(std::ranges::find(names, "modules.enable") == names.end());
  resident.flow.useDecider(resident.scripted);
  resident.scripted.next = candidate("modules.enable", 1.0);
  const auto outcome = resident.say("activa el módulo de productividad");
  CHECK(outcome.steps.empty());
  CHECK(resident.ran.empty());
}

TEST_CASE("an anonymous caller keeps no questions and no confirmations between turns")
{
  World world;
  world.context.userId = 0;
  REQUIRE(world.say("anota una tarea").question.has_value());
  const auto next = world.say("llamar al dentista");
  CHECK(world.ran.empty());
  CHECK(next.steps.empty());
}

TEST_CASE("the voice is told what was done and nothing else is added, with no tool list and no call syntax")
{
  Spoken spoken;
  spoken.script.replies = {"Listo, quedó agendada."};
  const auto output = spoken.sync("agéndame una reunión con Andrea mañana a las 5 de la tarde");
  REQUIRE(spoken.world.ran.size() == 1);
  CHECK(output.reply == "Listo, quedó agendada.");
  CHECK(output.executed.size() == 1);
  CHECK(output.hops == 1);
  REQUIRE(spoken.script.requests.size() == 1);
  const ChatRequest& request = spoken.script.requests.front();
  CHECK_FALSE(request.toolCallsAllowed);
  REQUIRE(request.messages.size() == 3);
  CHECK(request.messages.front().role == "system");
  CHECK(request.messages.front().content == "persona");
  CHECK(request.messages[1].role == "user");
  CHECK(request.messages.back().role == "system");
  CHECK(request.messages.back().content.find("Agendé «Reunión con Andrea» para " + tomorrowAtFive()) != std::string::npos);
  for (const auto& message : request.messages) {
    CHECK(message.content.find("List of tools") == std::string::npos);
    CHECK(message.content.find("tool_call") == std::string::npos);
  }
}

TEST_CASE("a question is answered by the system itself, without a generation, sync and streamed")
{
  Spoken sync;
  const auto output = sync.sync("agéndame una reunión con Andrea");
  CHECK(sync.script.requests.empty());
  CHECK(output.reply == turn_texts::slotQuestion({.tool = "calendar.create_event", .slot = "starts_at", .lang = "es"}));
  CHECK(output.generateMs == 0);

  Spoken streamed;
  const auto asked = streamed.stream("agéndame una reunión con Andrea");
  CHECK(streamed.script.requests.empty());
  CHECK(streamed.heard == asked.reply);
  CHECK(asked.emitted);
}

TEST_CASE("a plain conversation turn runs nothing and the reply is untouched")
{
  Spoken spoken;
  spoken.script.replies = {"Muy bien, gracias por preguntar."};
  const auto output = spoken.sync("hola Argus, cómo estás");
  CHECK(spoken.world.ran.empty());
  CHECK(output.executed.empty());
  CHECK(output.hops == 0);
  CHECK(output.reply == "Muy bien, gracias por preguntar.");
  REQUIRE(spoken.script.requests.size() == 1);
  CHECK(spoken.script.requests.front().messages.size() == 2);
}

TEST_CASE("a reply that claims something no tool did is never spoken, and a claim after a tool that worked is")
{
  Spoken none;
  none.script.replies = {"Guardé tu nota."};
  CHECK(none.sync("gracias").reply == reply_claims::honest("es"));
  CHECK(none.script.requests.size() == 1);

  Spoken worked;
  worked.script.replies = {"Listo, ya lo agendé para mañana."};
  CHECK(worked.sync("agéndame una reunión con Andrea mañana a las 5 de la tarde").reply == "Listo, ya lo agendé para mañana.");

  Spoken english;
  english.world.context.lang = "en";
  english.script.replies = {"I have created the task."};
  CHECK(english.sync("thanks").reply == reply_claims::honest("en"));
}

TEST_CASE("a preview is not a deed, so a claim of it is cut, and a tool that only reads does not make a claim of writing true")
{
  Spoken preview;
  preview.adapter.flow().useDecider(preview.world.scripted);
  turn::Candidate cancel = candidate("calendar.cancel_event", 1.0);
  cancel.arguments["title"] = "Cena con Marta";
  preview.world.scripted.next = cancel;
  preview.script.replies = {"Listo, cancelé la cena con Marta."};
  const auto output = preview.sync("cancela la cena con Marta");
  CHECK(output.reply == reply_claims::honest("es"));
  REQUIRE(preview.script.requests.size() == 1);
  CHECK(preview.script.requests.front().messages.back().content.find(std::string(kCode)) == std::string::npos);

  Spoken reading;
  reading.adapter.flow().useDecider(reading.world.scripted);
  reading.world.scripted.next = candidate("task.list", 1.0);
  reading.script.replies = {"Agendé la reunión con Andrea."};
  CHECK(reading.sync("¿Qué tengo mañana en la agenda?").reply == reply_claims::honest("es"));
  CHECK(reading.world.ran.size() == 1);
}

TEST_CASE("a streamed claim is cut at its sentence, and what came before it was already spoken")
{
  Spoken spoken;
  spoken.script.chunk = 4;
  spoken.script.replies = {"Hola, Ana. Creo una reunión con Andrea para el jueves. Confirmado."};
  const auto output = spoken.stream("gracias por todo");
  CHECK(spoken.heard == "Hola, Ana. " + reply_claims::honest("es"));
  CHECK(output.reply == spoken.heard);
  REQUIRE_FALSE(spoken.pieces.empty());
  CHECK(spoken.pieces.front() == "Hola, Ana.");

  Spoken worked;
  worked.script.chunk = 5;
  worked.script.replies = {"Listo, ya lo agendé para mañana. Confirmado."};
  const auto done = worked.stream("agéndame una reunión con Andrea mañana a las 5 de la tarde");
  CHECK(worked.heard == "Listo, ya lo agendé para mañana. Confirmado.");
  CHECK(done.reply == worked.heard);
}

TEST_CASE("a prefill-only turn primes the prompt a real turn starts from and decides nothing")
{
  Spoken spoken;
  auto input = spoken.input();
  input.prefillOnly = true;
  std::vector<ChatMessage> primed{{.role = "system", .content = "persona"}, {.role = "assistant", .content = "Hola, soy Argus."}};
  const TokenCallback onToken = [](const std::string&, bool) {};
  spoken.adapter.chatWithToolsStream({.input = input, .history = primed, .onToken = onToken});
  REQUIRE(spoken.script.requests.size() == 1);
  CHECK(spoken.script.requests.front().prefillOnly);
  CHECK(spoken.world.ran.empty());

  spoken.script.replies = {"Hola."};
  std::vector<ChatMessage> turn = primed;
  turn.push_back({.role = "user", .content = "hola"});
  spoken.adapter.chatWithTools(spoken.input(), turn);
  REQUIRE(spoken.script.requests.size() == 2);
  const auto& primedMessages = spoken.script.requests[0].messages;
  const auto& turnMessages = spoken.script.requests[1].messages;
  REQUIRE(turnMessages.size() == primedMessages.size() + 1);
  for (std::size_t i = 0; i < primedMessages.size(); ++i) {
    CHECK(turnMessages[i].role == primedMessages[i].role);
    CHECK(turnMessages[i].content == primedMessages[i].content);
  }
}

TEST_CASE("a title is what is left of the sentence once the verb, the noun and the time are taken out")
{
  const slots::RuleText text;
  const auto title = text.extract({.tool = "calendar.create_event",
                                   .field = "title",
                                   .utterance = "agéndame una reunión con Andrea mañana a las 5 de la tarde",
                                   .lang = "es"});
  CHECK(titled(title) == "Reunión con Andrea");
  CHECK_FALSE(text.extract({.tool = "calendar.create_event", .field = "project", .utterance = "agéndame una reunión", .lang = "es"}).has_value());
  CHECK_FALSE(text.extract({.tool = "task.create", .field = "title", .utterance = "anota una tarea", .lang = "es"}).has_value());
}

TEST_CASE("a module slot is filled from the names the snapshot carries and asked for when none is heard")
{
  World world;
  ModuleFlag productivity;
  productivity.id = "productivity";
  productivity.name = {.es = "Productividad", .en = "Productivity"};
  const ModuleSnapshot modules({productivity});
  const auto handle = world.registry.find("modules.enable");
  REQUIRE(handle != nullptr);
  const std::vector<std::string> fields{"module"};
  tools::ToolContext context = world.context;

  context.utterance = "la de productividad";
  const auto heard = slots::fill({.spec = handle->spec,
                                  .fields = fields,
                                  .arguments = Json::Value(Json::objectValue),
                                  .context = context,
                                  .now = world.now,
                                  .text = world.text,
                                  .modules = modules,
                                  .answering = true});
  CHECK(heard.missing.empty());
  CHECK(heard.arguments["module"].asString() == "productivity");

  context.utterance = "esa de ahí";
  const auto unheard = slots::fill({.spec = handle->spec,
                                    .fields = fields,
                                    .arguments = Json::Value(Json::objectValue),
                                    .context = context,
                                    .now = world.now,
                                    .text = world.text,
                                    .modules = modules,
                                    .answering = true});
  REQUIRE(unheard.missing.size() == 1);
  CHECK(unheard.missing.front() == "module");
}

TEST_CASE("the model slot falls back to the rules and never loads the model inside a turn")
{
  ExtractionService extraction;
  const turn::ModelText text(extraction);
  const auto title =
      text.extract({.tool = "task.create", .field = "title", .utterance = "anota una tarea: llamar al dentista", .lang = "es"});
  CHECK(titled(title) == "Llamar al dentista");
  CHECK_FALSE(text.extract({.tool = "task.create", .field = "title", .utterance = "anota una tarea", .lang = "es"}).has_value());
  CHECK_FALSE(extraction.isLoaded());
}

namespace
{
class RankedClassifier final : public intent::IIntentClassifier
{
public:
  explicit RankedClassifier(std::vector<intent::IntentHit> hits) : hits_(std::move(hits)) {}

  [[nodiscard]] bool isLoaded() const override { return true; }
  [[nodiscard]] std::vector<intent::IntentHit> score(const std::string&) const override { return hits_; }

private:
  std::vector<intent::IntentHit> hits_;
};

struct Moment
{
  int day{0};
  int hour{0};
  int minute{0};
};

std::int64_t at(const Moment& moment)
{
  std::tm local{};
  local.tm_year = 2026 - 1900;
  local.tm_mon = 9;
  local.tm_mday = moment.day;
  local.tm_hour = moment.hour;
  local.tm_min = moment.minute;
  local.tm_isdst = -1;
  return static_cast<std::int64_t>(std::mktime(&local));
}

constexpr std::string_view kBetween = "el sofá del salón es nuevo";
}

TEST_CASE("two tools that are close are asked about in one question, and the answer picks one")
{
  World world;
  PhraseCatalog catalog;
  catalog.build();
  const RankedClassifier classifier({{.intent = intent::ToolIntent::MemorySave, .score = 0.70F},
                                     {.intent = intent::ToolIntent::MemoryRecall, .score = 0.62F},
                                     {.intent = intent::ToolIntent::None, .score = 0.01F}});
  const IntentRouter router({.catalog = catalog, .model = classifier, .recurrent = {}});
  const turn::RouterDecider decider(&router);
  const std::string utterance(kBetween);
  const auto decision = must(decider.decide({.utterance = utterance, .lang = "es", .offered = world.offered, .modules = world.audience.modules}));
  CHECK(decision.tool == "memory.remember");
  CHECK_FALSE(decision.confident);
  CHECK_FALSE(decision.exact);
  REQUIRE(decision.runnerUp.has_value());
  CHECK(decision.runnerUp.value_or(turn::Pick{}).tool == "memory.recall");
  CHECK(decision.runnerUp.value_or(turn::Pick{}).confidence == doctest::Approx(0.62).epsilon(0.001));

  world.flow.useDecider(decider);
  const auto asked = world.say(utterance);
  CHECK(world.ran.empty());
  CHECK(said(asked) == turn_texts::chooseQuestion({.first = "memory.remember", .second = "memory.recall", .lang = "es"}));

  const auto other = world.say("la otra");
  REQUIRE(world.ran.size() == 1);
  CHECK(world.ran.front().name == "memory.recall");
  CHECK(world.ran.front().arguments["query"].asString() == utterance);
  CHECK_FALSE(other.wrote);

  REQUIRE(world.say(utterance).question.has_value());
  const auto yes = world.say("sí");
  REQUIRE(world.ran.size() == 2);
  CHECK(world.ran.back().name == "memory.remember");
  CHECK(yes.wrote);

  REQUIRE(world.say(utterance).question.has_value());
  CHECK(findingOf(world.say("no, ninguna"), turn::FindingKind::Declined) != nullptr);
  CHECK(world.ran.size() == 2);
}

TEST_CASE("the choice is asked in English too and a sentence that answers neither is decided on its own")
{
  World world;
  world.context.lang = "en";
  world.flow.useDecider(world.scripted);
  turn::Candidate close = candidate("task.create", 0.95);
  close.runnerUp = turn::Pick{.tool = "calendar.create_event", .arguments = Json::Value(Json::objectValue), .fill = {}, .confidence = 0.92};
  world.scripted.next = close;
  const auto asked = world.say("call the dentist tomorrow");
  CHECK(said(asked) == "Do you want me to add it as a task or schedule it?");
  world.scripted.next.reset();
  const auto neither = world.say("what is the weather like");
  CHECK(neither.steps.empty());
  CHECK(world.ran.empty());
  CHECK(world.say("the other one").steps.empty());
}

TEST_CASE("a write that only one decider believes is asked about, and a second signal lets it through")
{
  World world;
  ScriptedDecider witness;
  witness.name = "witness";
  world.flow.useDecider(world.scripted);
  world.scripted.name = "laya";
  turn::Candidate write = candidate("task.create", 0.97);
  write.decider = "laya";
  write.exact = false;
  write.arguments["title"] = "llamar al dentista";
  world.scripted.next = write;

  const auto asked = world.say("llamar al dentista");
  CHECK(world.ran.empty());
  CHECK(said(asked) == turn_texts::confirmQuestion({.tool = "task.create", .lang = "es", .details = {.title = "llamar al dentista", .when = {}, .module = {}}}));
  const auto yes = world.say("sí");
  REQUIRE(world.ran.size() == 1);
  CHECK(yes.wrote);
  CHECK(world.ran.front().context.utterance == "llamar al dentista");

  world.flow.useWitnesses({&witness});
  witness.next = candidate("task.list", 1.0);
  CHECK(world.say("llamar al dentista").question.has_value());
  CHECK(world.ran.size() == 1);
  world.say("no");

  witness.next = candidate("task.create", 1.0);
  const auto agreed = world.say("llamar al dentista");
  CHECK_FALSE(agreed.question.has_value());
  CHECK(world.ran.size() == 2);

  world.flow.useWitnesses({});
  world.flow.useSecondSignal([](const turn::SecondOpinion& opinion) { return opinion.candidate.tool == "task.create"; });
  CHECK_FALSE(world.say("llamar al dentista").question.has_value());
  CHECK(world.ran.size() == 3);
  world.flow.useSecondSignal([](const turn::SecondOpinion&) { return false; });
  CHECK(world.say("llamar al dentista").question.has_value());
  CHECK(world.ran.size() == 3);
}

TEST_CASE("a read needs no second signal, but a decider that doubts itself is asked about even above the threshold")
{
  World world;
  world.flow.useDecider(world.scripted);
  world.scripted.name = "laya";
  turn::Candidate read = candidate("task.list", 0.97);
  read.decider = "laya";
  read.exact = false;
  world.scripted.next = read;
  CHECK_FALSE(world.say("qué tareas tengo").question.has_value());
  REQUIRE(world.ran.size() == 1);

  read.confident = false;
  world.scripted.next = read;
  const auto doubtful = world.say("qué tareas tengo");
  CHECK(said(doubtful) == turn_texts::confirmQuestion({.tool = "task.list", .lang = "es"}));
  CHECK(world.ran.size() == 1);
}

TEST_CASE("the thresholds of the decider that spoke are the ones that count")
{
  World world;
  world.flow.useDecider(world.scripted);
  world.scripted.name = "laya";
  turn::PolicySet policies({.act = 0.90, .ask = 0.60, .margin = 0.10});
  policies.set("laya", {.act = 0.99, .ask = 0.97, .margin = 0.0});
  world.flow.usePolicies(policies);
  turn::Candidate read = candidate("task.list", 0.95);
  read.decider = "laya";
  world.scripted.next = read;
  CHECK_FALSE(world.say("qué tareas tengo").question.has_value());
  CHECK(world.ran.empty());

  read.confidence = 0.98;
  world.scripted.next = read;
  CHECK(world.say("qué tareas tengo").question.has_value());
  read.confidence = 1.0;
  world.scripted.next = read;
  CHECK_FALSE(world.say("qué tareas tengo").question.has_value());
  CHECK(world.ran.size() == 1);
}

TEST_CASE("a question says what it is about, with the time spoken the way it is asked")
{
  World world;
  world.flow.useDecider(world.scripted);
  turn::Candidate maybe = candidate("calendar.create_event", 0.70);
  maybe.fill = {"title", "starts_at"};
  world.scripted.next = maybe;
  const auto asked = world.say("agéndame una reunión con Andrea mañana a las 5 de la tarde");
  CHECK(said(asked) == "¿Quieres que agende «Reunión con Andrea» para mañana a las 5 de la tarde?");
  CHECK(world.ran.empty());
  world.scripted.next.reset();
  const auto yes = world.say("sí");
  REQUIRE(world.ran.size() == 1);
  CHECK(world.ran.front().arguments["title"].asString() == "Reunión con Andrea");
  CHECK(world.ran.front().arguments["starts_at"].asString().starts_with(tomorrowAtFive()));
  CHECK(yes.wrote);
}

TEST_CASE("a time is spoken as today, tomorrow or the weekday, in the 12-hour clock of each language")
{
  const std::int64_t now = at({.day = 6, .hour = 10, .minute = 0});
  const auto when = [now](std::int64_t moment, std::string_view lang) {
    return turn_texts::spokenWhen({.iso = iso_time::format(moment), .now = now, .lang = lang});
  };
  CHECK(when(at({.day = 6, .hour = 15, .minute = 30}), "es") == "hoy a las 3:30 de la tarde");
  CHECK(when(at({.day = 6, .hour = 15, .minute = 30}), "en") == "today at 3:30 PM");
  CHECK(when(at({.day = 7, .hour = 9, .minute = 0}), "es") == "mañana a las 9 de la mañana");
  CHECK(when(at({.day = 7, .hour = 9, .minute = 0}), "en") == "tomorrow at 9 AM");
  CHECK(when(at({.day = 9, .hour = 13, .minute = 0}), "es") == "el viernes 9 a la 1 de la tarde");
  CHECK(when(at({.day = 9, .hour = 13, .minute = 0}), "en") == "on Friday the 9 at 1 PM");
  CHECK(when(at({.day = 6, .hour = 21, .minute = 5}), "es") == "hoy a las 9:05 de la noche");
  CHECK(turn_texts::spokenWhen({.iso = "not a time", .now = now, .lang = "es"}).empty());
}

TEST_CASE("a question falls back to the plain one when what it would say is missing")
{
  CHECK(turn_texts::confirmQuestion({.tool = "calendar.create_event", .lang = "es", .details = {.title = "Cena", .when = {}, .module = {}}}) ==
        "¿Quieres que agende un evento?");
  CHECK(turn_texts::confirmQuestion({.tool = "modules.enable", .lang = "en", .details = {.title = {}, .when = {}, .module = "Productivity"}}) ==
        "Shall I turn on the Productivity module?");
  CHECK(turn_texts::confirmQuestion({.tool = "tool.unknown", .lang = "es"}) == "¿Quieres que lo haga?");
  CHECK(turn_texts::chooseQuestion({.first = "task.create", .second = "memory.remind", .lang = "es"}) ==
        "¿Quieres que lo anote como tarea o que te lo recuerde?");
}

TEST_CASE("the other one is a phrase of the language and nothing else is")
{
  CHECK(slots::namesOther("la otra"));
  CHECK(slots::namesOther("no, mejor el otro"));
  CHECK(slots::namesOther("the other one please"));
  CHECK(slots::namesOther("la segunda opción"));
  CHECK_FALSE(slots::namesOther("sí"));
  CHECK_FALSE(slots::namesOther("agéndame una reunión"));
}

namespace
{
struct AppTurn
{
  Spoken spoken;
  std::vector<std::string> actions;

  AppTurn()
  {
    spoken.world.add(tool_stubs::appAction({.name = "app.set_guard_mode", .capability = "guard.mode.set", .module = "surveillance"}));
    spoken.world.add(tool_stubs::appAction({.name = "app.show_camera", .capability = "camera.view", .module = "surveillance"}));
    spoken.world.context.emitAction = [this](const std::string& name, const Json::Value&) { actions.push_back(name); };
  }
};
}

TEST_CASE("an app command is decided and run before anything is said, and a turn without app tools runs none")
{
  AppTurn turn;
  turn.spoken.script.replies = {"Listo, modo noche activado."};
  const auto output = turn.spoken.sync("Pon la vigilancia en modo noche.");
  REQUIRE(turn.actions.size() == 1);
  CHECK(turn.actions.front() == "app.set_guard_mode");
  REQUIRE(turn.spoken.script.requests.size() == 1);
  CHECK_FALSE(turn.spoken.script.requests.front().toolCallsAllowed);
  CHECK(output.reply == "Listo, modo noche activado.");

  Spoken bare;
  bare.script.replies = {"No puedo cambiarla desde aquí."};
  bare.world.context.emitAction = [&turn](const std::string& name, const Json::Value&) { turn.actions.push_back(name); };
  CHECK(bare.sync("Pon la vigilancia en modo noche.").reply == "No puedo cambiarla desde aquí.");
  CHECK(turn.actions.size() == 1);
}

TEST_CASE("a streamed reply that claims an app action no tool ran is replaced, and a question about the app is not touched")
{
  AppTurn claim;
  claim.spoken.script.replies = {"Cambié la vigilancia a modo noche."};
  const auto output = claim.spoken.stream("oye la vigilancia esta noche que esté atenta a todo por favor");
  CHECK(claim.actions.empty());
  CHECK(claim.spoken.heard == reply_claims::honest("es"));
  CHECK(output.reply == claim.spoken.heard);

  AppTurn question;
  question.spoken.script.replies = {"Está en modo noche, lo cambiaste anoche."};
  question.spoken.stream("¿en qué modo está la vigilancia?");
  CHECK(question.actions.empty());
  CHECK(question.spoken.heard == "Está en modo noche, lo cambiaste anoche.");
}

TEST_CASE("a tool that failed does not make a later claim true, and what it said is what the speaker is told")
{
  Spoken failed;
  failed.world.createOk = false;
  failed.script.replies = {"Listo, ya lo agendé."};
  const auto output = failed.sync("agéndame una reunión con Andrea mañana a las 5 de la tarde");
  CHECK(failed.world.ran.size() == 1);
  CHECK(output.reply == reply_claims::honest("es"));
  CHECK(output.executed.size() == 1);
  REQUIRE(failed.script.requests.size() == 1);
  CHECK(failed.script.requests.front().messages.back().content.find("No pude agendar.") != std::string::npos);

  Spoken english;
  english.world.context.lang = "en";
  english.script.replies = {"Done, I've scheduled it."};
  const auto spoken = english.sync("Schedule a meeting with Andrea tomorrow at 5 pm");
  CHECK(spoken.reply == "Done, I've scheduled it.");
}

namespace
{
std::uint64_t counted(const World& world, std::string_view decider, std::string_view family, std::string_view verdict)
{
  std::uint64_t total = 0;
  for (const auto& tally : world.flow.decisions())
    if (tally.key.decider == decider && tally.key.family == family && tally.key.verdict == verdict)
      total += tally.count;
  return total;
}

turn::PolicySet witnessOnlyRules()
{
  turn::PolicySet policies({.act = 0.90, .ask = 0.60, .margin = 0.10});
  policies.set("rules", {.act = 0.90, .ask = 0.60, .margin = 0.10, .witnessOnly = true});
  return policies;
}
}

TEST_CASE("a decider marked witness-only asks for every write, whatever its confidence, and still acts on a read")
{
  World world;
  world.flow.usePolicies(witnessOnlyRules());
  const auto asked = world.say("agéndame una reunión con Andrea mañana a las 5 de la tarde");
  CHECK(world.ran.empty());
  CHECK(said(asked) == "¿Quieres que agende «Reunión con Andrea» para mañana a las 5 de la tarde?");
  CHECK_FALSE(asked.wrote);
  const auto yes = world.say("sí");
  REQUIRE(world.ran.size() == 1);
  CHECK(yes.wrote);

  world.flow.useDecider(world.scripted);
  world.scripted.name = "rules";
  world.scripted.next = candidate("task.list", 1.0);
  world.scripted.next->decider = "rules";
  CHECK_FALSE(world.say("qué tareas tengo").question.has_value());
  CHECK(world.ran.size() == 2);
  world.scripted.next = candidate("task.create", 1.0);
  world.scripted.next->decider = "rules";
  world.scripted.next->arguments["title"] = "llamar al dentista";
  CHECK(world.say("anota llamar al dentista").question.has_value());
  CHECK(world.ran.size() == 2);
}

TEST_CASE("the same decider acts alone on a write when it is not marked, and the mark is per decider")
{
  World plain;
  const auto acted = plain.say("agéndame una reunión con Andrea mañana a las 5 de la tarde");
  CHECK_FALSE(acted.question.has_value());
  CHECK(plain.ran.size() == 1);

  World other;
  turn::PolicySet policies({.act = 0.90, .ask = 0.60, .margin = 0.10});
  policies.set("router", {.act = 0.90, .ask = 0.60, .margin = 0.10, .witnessOnly = true});
  other.flow.usePolicies(policies);
  CHECK_FALSE(other.say("agéndame una reunión con Andrea mañana a las 5 de la tarde").question.has_value());
  CHECK(other.ran.size() == 1);
}

TEST_CASE("a witness-only decider is the second signal another decider's write needs")
{
  World world;
  world.flow.usePolicies(witnessOnlyRules());
  world.flow.useDecider(world.scripted);
  world.flow.useWitnesses({&world.rules});
  world.scripted.name = "laya";
  turn::Candidate write = candidate("calendar.create_event", 0.97);
  write.decider = "laya";
  write.exact = false;
  write.fill = {"title", "starts_at"};
  world.scripted.next = write;

  const auto agreed = world.say("agéndame una reunión con Andrea mañana a las 5 de la tarde");
  CHECK_FALSE(agreed.question.has_value());
  REQUIRE(world.ran.size() == 1);
  CHECK(world.ran.front().arguments["title"].asString() == "Reunión con Andrea");

  const auto alone = world.say("una reunión con Andrea mañana a las 5 de la tarde");
  CHECK(alone.question.has_value());
  CHECK(world.ran.size() == 1);
}

TEST_CASE("every decision is counted by decider, family, language and what became of it, and written to the log without the words")
{
  World world;
  world.say("agéndame una reunión con Andrea mañana a las 5 de la tarde");
  world.say("agéndame una reunión con Marta mañana a las 5 de la tarde");
  CHECK(counted(world, "rules", "calendar", "act") == 2);

  world.flow.usePolicies(witnessOnlyRules());
  world.say("agéndame una reunión con Pedro mañana a las 5 de la tarde");
  CHECK(counted(world, "rules", "calendar", "guard") == 1);
  CHECK(counted(world, "rules", "calendar", "act") == 2);

  world.context.lang = "en";
  world.say("Schedule a meeting with Andrea tomorrow at 5 pm");
  bool english = false;
  for (const auto& tally : world.flow.decisions()) {
    CHECK(tally.key.exact);
    english = english || tally.key.lang == "en";
  }
  CHECK(english);
  CHECK(world.flow.decisions().size() == 3);
}

TEST_CASE("the second signal and the decider are given the words, the tool and its arguments, the language and the assistant's last turn")
{
  World world;
  world.flow.useDecider(world.scripted);
  world.scripted.name = "laya";
  turn::Candidate write = candidate("task.create", 0.97);
  write.decider = "laya";
  write.exact = false;
  write.arguments["title"] = "llamar al dentista";
  world.scripted.next = write;

  struct Seen
  {
    std::string utterance;
    std::string previous;
    std::string lang;
    std::string tool;
    std::string title;
    int calls{0};
  };
  Seen seen;
  world.flow.useSecondSignal([&seen](const turn::SecondOpinion& opinion) {
    seen = {.utterance = std::string(opinion.input.utterance),
            .previous = std::string(opinion.input.previousAssistant),
            .lang = std::string(opinion.input.lang),
            .tool = opinion.candidate.tool,
            .title = opinion.candidate.arguments["title"].asString(),
            .calls = seen.calls + 1};
    return true;
  });
  world.previous = "¿Algo más que quieras que haga?";
  world.say("sí, llamar al dentista");
  CHECK(seen.calls == 1);
  CHECK(seen.utterance == "sí, llamar al dentista");
  CHECK(seen.previous == "¿Algo más que quieras que haga?");
  CHECK(seen.lang == "es");
  CHECK(seen.tool == "task.create");
  CHECK(seen.title == "llamar al dentista");
  CHECK(world.scripted.heardPrevious == "¿Algo más que quieras que haga?");
  CHECK(world.ran.size() == 1);
}

TEST_CASE("the adapter hands the decider the assistant turn that came before the user's words")
{
  Spoken spoken;
  spoken.adapter.flow().useDecider(spoken.world.scripted);
  spoken.script.replies = {"Dime."};
  std::vector<ChatMessage> history{{.role = "system", .content = "persona"},
                                   {.role = "assistant", .content = "Hola, soy Argus."},
                                   {.role = "user", .content = "hola"},
                                   {.role = "assistant", .content = "¿Qué necesitas?"},
                                   {.role = "user", .content = "una cosa"}};
  spoken.adapter.chatWithTools(spoken.input(), history);
  CHECK(spoken.world.scripted.heardPrevious == "¿Qué necesitas?");
}

TEST_CASE("the read-only tools are exactly the list in tool-effects.json and every other tool is a write")
{
  const std::vector<std::string> expected{"calendar.list_events", "task.list", "project.list", "modules.list", "modules.explain",
                                          "reminder.list", "memory.recall", "app.open", "app.show_camera"};
  CHECK(turn::kReadOnlyTools.size() == expected.size());
  for (const std::string& name : expected)
    CHECK(turn::isReadOnlyTool(name));
  for (const std::string name : {"app.set_guard_mode", "calendar.create_event", "calendar.cancel_event", "task.create", "task.complete",
                                 "project.create", "modules.enable", "modules.disable", "modules.request", "modules.open_purge_screen",
                                 "memory.remember", "memory.remind", "memory.forget", "tool.unknown"})
    CHECK_FALSE(turn::isReadOnlyTool(name));
}

TEST_CASE("the guard of a learned decider is the question it answers itself, now against its own threshold")
{
  World world;
  world.flow.useDecider(world.scripted);
  world.scripted.name = "laya";
  turn::PolicySet policies({.act = 0.90, .ask = 0.60, .margin = 0.10});
  policies.set("laya", {.act = 0.90, .ask = 0.60, .margin = 0.10, .nowMin = 0.80});
  world.flow.usePolicies(policies);
  const auto write = [](std::optional<double> now) {
    turn::Candidate candidate = ::candidate("task.create", 0.97);
    candidate.decider = "laya";
    candidate.exact = false;
    candidate.arguments["title"] = "llamar al dentista";
    candidate.now = now;
    return candidate;
  };

  world.scripted.next = write(0.92);
  CHECK_FALSE(world.say("llamar al dentista").question.has_value());
  CHECK(world.ran.size() == 1);

  world.scripted.next = write(0.50);
  CHECK(world.say("llamar al dentista").question.has_value());
  CHECK(world.ran.size() == 1);
  world.scripted.next.reset();
  world.say("no");

  world.scripted.next = write(std::nullopt);
  CHECK(world.say("llamar al dentista").question.has_value());
  CHECK(world.ran.size() == 1);
  world.scripted.next.reset();
  world.say("no");

  ScriptedDecider witness;
  witness.name = "witness";
  witness.next = candidate("task.create", 1.0);
  world.flow.useWitnesses({&witness});
  world.scripted.next = write(0.50);
  CHECK(world.say("llamar al dentista").question.has_value());
  CHECK(world.ran.size() == 1);
  world.scripted.next.reset();
  world.say("no");

  turn::Candidate read = candidate("task.list", 0.97);
  read.decider = "laya";
  read.exact = false;
  world.scripted.next = read;
  CHECK_FALSE(world.say("qué tareas tengo").question.has_value());
  CHECK(world.ran.size() == 2);

  const auto counts = world.flow.decisions();
  const auto guarded = std::ranges::find_if(counts, [](const turn::DecisionCount& tally) { return tally.key.verdict == "guard"; });
  REQUIRE(guarded != counts.end());
  CHECK(guarded->count == 3);
}

TEST_CASE("changing the guard mode is a write that waits, whatever the tool says about itself, and showing a camera never waits")
{
  World world;
  auto guardMode = tool_stubs::appAction({.name = "app.set_guard_mode", .capability = "guard.mode.set", .module = "surveillance"});
  guardMode.spec.annotations.readOnly = true;
  world.add(std::move(guardMode));
  world.add(tool_stubs::appAction({.name = "app.show_camera", .capability = "camera.view", .module = "surveillance"}));
  world.refresh();
  std::vector<std::string> actions;
  world.context.emitAction = [&actions](const std::string& name, const Json::Value&) { actions.push_back(name); };
  world.flow.useDecider(world.scripted);
  world.scripted.name = "laya";

  turn::Candidate mode = candidate("app.set_guard_mode", 0.97);
  mode.decider = "laya";
  mode.exact = false;
  mode.arguments["mode"] = "night";
  world.scripted.next = mode;
  const auto asked = world.say("pon la vigilancia en modo noche");
  CHECK(said(asked) == turn_texts::confirmQuestion({.tool = "app.set_guard_mode", .lang = "es"}));
  CHECK(actions.empty());
  world.scripted.next.reset();
  const auto yes = world.say("sí");
  CHECK(actions.size() == 1);
  CHECK(yes.wrote);

  turn::Candidate camera = candidate("app.show_camera", 0.97);
  camera.decider = "laya";
  camera.exact = false;
  world.scripted.next = camera;
  CHECK_FALSE(world.say("muéstrame la cámara").question.has_value());
  REQUIRE(actions.size() == 2);
  CHECK(actions.back() == "app.show_camera");
}

namespace
{
std::vector<std::string> toolsRun(const World& world)
{
  std::vector<std::string> names;
  names.reserve(world.ran.size());
  for (const auto& call : world.ran)
    names.push_back(call.name);
  return names;
}
}

TEST_CASE("a task with several projects asks which one, reads the answer for that slot only and completes the held task")
{
  World world;
  world.projects = {"Casa", "Trabajo", "Viaje"};
  const auto asked = world.say("anota una tarea: llamar al dentista");
  CHECK(said(asked) == "¿En cuál proyecto va? Casa, Trabajo o Viaje.");
  CHECK_FALSE(asked.wrote);
  CHECK(asked.findings.empty());
  CHECK(world.ran.size() == 1);

  const auto done = world.say("el de la casa");
  REQUIRE(world.ran.size() == 2);
  CHECK(world.ran.back().name == "task.create");
  CHECK(world.ran.back().arguments["project"].asString() == "Casa");
  CHECK(world.ran.back().arguments["title"].asString() == "Llamar al dentista");
  CHECK(done.wrote);
  CHECK(world.say("el de la casa").steps.empty());
}

TEST_CASE("with no project the system offers to create one by its name, and only on a yes")
{
  World world;
  world.projects.clear();
  CHECK(said(world.say("anota una tarea: llamar al dentista")) ==
        "Todavía no tienes proyectos. ¿Cómo quieres llamar al proyecto nuevo para esta tarea?");
  CHECK(said(world.say("Hogar")) == "¿Creo el proyecto «Hogar» y anoto la tarea ahí?");
  CHECK(toolsRun(world) == std::vector<std::string>{"task.create"});

  const auto yes = world.say("sí");
  CHECK(toolsRun(world) == std::vector<std::string>{"task.create", "project.create", "task.create"});
  CHECK(world.ran[1].arguments["name"].asString() == "Hogar");
  CHECK(world.ran[2].arguments["project"].asString() == "Hogar");
  CHECK(world.ran[2].arguments["title"].asString() == "Llamar al dentista");
  CHECK(yes.wrote);
  CHECK(yes.findings.size() == 2);
}

TEST_CASE("none of the projects, or create one, leads to the same offer and never creates silently")
{
  World none;
  none.projects = {"Casa", "Trabajo"};
  REQUIRE(none.say("anota una tarea: llamar al dentista").question.has_value());
  CHECK(said(none.say("ninguno")) == "¿Cómo se llama el proyecto nuevo?");
  CHECK(said(none.say("se llama Hogar")) == "¿Creo el proyecto «Hogar» y anoto la tarea ahí?");
  CHECK(findingOf(none.say("no, déjalo"), turn::FindingKind::Declined) != nullptr);
  CHECK(toolsRun(none) == std::vector<std::string>{"task.create"});
  CHECK(none.say("sí").steps.empty());

  World named;
  named.projects = {"Casa", "Trabajo"};
  REQUIRE(named.say("anota una tarea: llamar al dentista").question.has_value());
  CHECK(said(named.say("crea uno llamado Hogar")) == "¿Creo el proyecto «Hogar» y anoto la tarea ahí?");
  CHECK(toolsRun(named) == std::vector<std::string>{"task.create"});
  named.say("sí");
  CHECK(toolsRun(named) == std::vector<std::string>{"task.create", "project.create", "task.create"});
  CHECK(named.ran.back().arguments["project"].asString() == "Hogar");
}

TEST_CASE("an answer that names no project is asked again once, and a no drops the held task")
{
  World world;
  world.projects = {"Casa", "Trabajo"};
  REQUIRE(world.say("anota una tarea: llamar al dentista").question.has_value());
  CHECK(said(world.say("ese")) == "¿En cuál proyecto va? Casa o Trabajo.");
  CHECK(said(world.say("ese")) == turn_texts::misunderstood("es"));
  CHECK(world.ran.size() == 1);
  CHECK(world.say("el de la casa").steps.empty());

  World dropped;
  dropped.projects = {"Casa", "Trabajo"};
  REQUIRE(dropped.say("anota una tarea: llamar al dentista").question.has_value());
  CHECK(findingOf(dropped.say("no"), turn::FindingKind::Declined) != nullptr);
  CHECK(dropped.say("el de la casa").steps.empty());
  CHECK(dropped.ran.size() == 1);
}

TEST_CASE("a new command while a project is asked for replaces the question")
{
  World world;
  world.projects = {"Casa", "Trabajo"};
  REQUIRE(world.say("anota una tarea: llamar al dentista").question.has_value());
  const auto fresh = world.say("agéndame una reunión con Andrea mañana a las 5 de la tarde");
  CHECK_FALSE(fresh.question.has_value());
  CHECK(world.ran.back().name == "calendar.create_event");
}

TEST_CASE("the same project question is asked and answered in English")
{
  World world;
  world.context.lang = "en";
  world.projects = {"Home", "Work"};
  CHECK(said(world.say("add a task: call the dentist")) == "Which project is it for? Home or Work.");
  const auto done = world.say("the home one");
  REQUIRE(world.ran.size() == 2);
  CHECK(world.ran.back().arguments["project"].asString() == "Home");
  CHECK(done.wrote);

  World none;
  none.context.lang = "en";
  none.projects.clear();
  CHECK(said(none.say("add a task: call the dentist")) == "You have no projects yet. What should I call the new project for this task?");
  CHECK(said(none.say("Garden")) == "Shall I create the project “Garden” and add the task there?");
  none.say("yes");
  CHECK(toolsRun(none) == std::vector<std::string>{"task.create", "project.create", "task.create"});
  CHECK(none.ran.back().arguments["project"].asString() == "Garden");

  World another;
  another.context.lang = "en";
  another.projects = {"Home", "Work"};
  REQUIRE(another.say("add a task: call the dentist").question.has_value());
  CHECK(said(another.say("none of them")) == "What is the new project called?");
  CHECK(said(another.say("call it Garden")) == "Shall I create the project “Garden” and add the task there?");
}

TEST_CASE("when no decider found anything and no write ran, a claim of work done is cut on every path")
{
  Spoken sync;
  sync.script.replies = {"¡Hola! He ajustado la calefacción para que esté un par de grados más cálida."};
  CHECK(sync.sync("sube la calefacción un par de grados").reply == reply_claims::honest("es"));
  CHECK(sync.world.ran.empty());

  Spoken streamed;
  streamed.script.chunk = 6;
  streamed.script.replies = {"Esa información está guardada para tu seguridad."};
  streamed.stream("o sea el perro no puede comer chocolate nunca");
  CHECK(streamed.heard == reply_claims::honest("es"));

  Spoken offer;
  offer.script.replies = {"Claro, puedo activar la agenda ahora."};
  CHECK(offer.sync("quiero usar la agenda, ¿puedes activarla?").reply == reply_claims::honest("es"));

  Spoken plain;
  plain.script.replies = {"Estoy listo para ayudarte con lo que necesites."};
  CHECK(plain.sync("hola Argus").reply == "Estoy listo para ayudarte con lo que necesites.");
}

TEST_CASE("a provider that names no projects leaves its own refusal to be said, and nothing is held")
{
  World world;
  world.projects = {"Casa", "Trabajo"};
  world.listsProjects = false;
  const auto outcome = world.say("anota una tarea: llamar al dentista");
  CHECK_FALSE(outcome.question.has_value());
  REQUIRE(outcome.findings.size() == 1);
  CHECK(outcome.findings.front().kind == turn::FindingKind::Refused);
  CHECK(world.say("el de la casa").steps.empty());
}

TEST_CASE("the notifications panel opens by an app command in both languages, for every role, with a module off")
{
  const auto opened = [](UserRole role, const std::string& lang, const std::string& utterance) {
    Spoken spoken(role);
    spoken.world.add(tool_stubs::appAction({.name = "app.open", .capability = "notifications.read", .module = "core"}));
    spoken.world.productivityOff();
    spoken.world.context.lang = lang;
    std::vector<std::pair<std::string, Json::Value>> actions;
    spoken.world.context.emitAction = [&actions](const std::string& name, const Json::Value& arguments) {
      actions.emplace_back(name, arguments);
    };
    spoken.script.replies = {lang == "en" ? "Here they are." : "Aquí están."};
    const auto output = spoken.sync(utterance);
    return std::make_pair(actions, output.reply);
  };
  for (const UserRole role : {UserRole::Owner, UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
    const auto [spanish, spoken] = opened(role, "es", "abre las notificaciones");
    REQUIRE(spanish.size() == 1);
    CHECK(spanish.front().first == "app.open");
    CHECK(spanish.front().second["screen"].asString() == "notifications");
    CHECK_FALSE(spanish.front().second.isMember("module"));
    CHECK(spoken == "Aquí están.");
  }
  const auto novedades = opened(UserRole::Resident, "es", "muéstrame las novedades");
  REQUIRE(novedades.first.size() == 1);
  CHECK(novedades.first.front().second["screen"].asString() == "notifications");
  const auto english = opened(UserRole::Guest, "en", "open my notifications");
  REQUIRE(english.first.size() == 1);
  CHECK(english.first.front().second["screen"].asString() == "notifications");
  CHECK(english.second == "Here they are.");
  CHECK(opened(UserRole::Owner, "es", "tengo muchas notificaciones sin leer").first.empty());
}
