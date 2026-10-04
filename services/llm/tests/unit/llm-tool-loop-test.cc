#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/intent/services/intent-contracts.hxx>
#include <feature/intent/services/intent-router.hxx>
#include <feature/llm/services/lfm-adapter.hxx>
#include <feature/llm/services/tools/app-command.hxx>
#include <feature/llm/services/tools/app-tool-descriptors.hxx>
#include <feature/llm/services/tools/tool-registry.hxx>
#include <phrase/phrase-catalog.hxx>

#include <deque>
#include <string>
#include <utility>
#include <vector>

namespace
{

struct ScriptedEngine
{
  std::deque<std::string> replies;
  std::vector<ChatRequest> requests;

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
                  onToken(next(request), false);
                  onToken("", true);
                }};
  }
};

tools::ToolCall commandOf(const std::string& utterance)
{
  auto command = appCommandFor(utterance);
  REQUIRE(command.has_value());
  return std::move(command).value_or(tools::ToolCall{});
}

struct ProbeLog
{
  std::vector<tools::ToolCall> calls;
};

tools::ToolDescriptor probe(const std::string& name, ProbeLog& log)
{
  return {.name = name,
          .description = "probe",
          .arguments = {{.name = "text",
                         .type = "string",
                         .required = false,
                         .enumValues = {},
                         .description = ""}},
          .accessTable = TableName::Memory,
          .accessPermission = RolePermission::Create,
          .handler = [&log](const tools::ToolCall& call) {
            log.calls.push_back(call);
            tools::ToolResult result;
            result.ok = true;
            result.output = "Guardado.";
            return result;
          }};
}

class SilentClassifier final : public intent::IIntentClassifier
{
public:
  [[nodiscard]] bool isLoaded() const override { return false; }
  [[nodiscard]] std::vector<intent::IntentHit> score(const std::string&) const override { return {}; }
};

ToolChatInput loopInput(const std::vector<const tools::ToolDescriptor*>& offered)
{
  ToolChatInput input;
  input.systemPrompt = "Eres Argus.";
  input.tools = offered;
  input.role = UserRole::Resident;
  input.context = {.userId = 7,
                   .lang = "es",
                   .sessionId = "voice-7-1",
                   .channel = "tool_result",
                   .utterance = {},
                   .decided = false,
                   .emitAction = {}};
  input.maxHops = 3;
  return input;
}

constexpr std::string_view kCall =
    "<|tool_call_start|>[probe.save(text='mi hermana viene los domingos')]<|tool_call_end|>";

}

TEST_CASE("notes the voice session adds to a user turn are not the user's words")
{
  CHECK(LfmAdapter::spokenText("recuerda que mi hermana viene los domingos\n"
                               "(Tono: cálido y corto.)") ==
        "recuerda que mi hermana viene los domingos");
  CHECK(LfmAdapter::spokenText("hola\n(Tone: warm and short.)\n\n") == "hola");
  CHECK(LfmAdapter::spokenText("  dime (por favor) la hora ") ==
        "dime (por favor) la hora");
  CHECK(LfmAdapter::spokenText("(risas)") == "(risas)");
}

TEST_CASE("a call rendered for the history reads back as the same call")
{
  tools::ToolCall call;
  call.name = "memory.remember";
  call.arguments["text"] = "a Rodrigo no le gusta el 'pescado'\ncrudo";
  call.arguments["confidence"] = 0.5;
  const std::string rendered = LfmAdapter::renderToolCall(call);
  CHECK(rendered.starts_with("<|tool_call_start|>[memory.remember("));
  CHECK(rendered.ends_with(")]<|tool_call_end|>"));
  const auto parsed = LfmAdapter::parseToolCalls(rendered);
  REQUIRE(parsed.size() == 1);
  CHECK(parsed[0].name == "memory.remember");
  CHECK(parsed[0].arguments["text"].asString() ==
        "a Rodrigo no le gusta el 'pescado'\ncrudo");
  CHECK(parsed[0].arguments["confidence"].asDouble() == doctest::Approx(0.5));

  const auto bracketed = LfmAdapter::parseToolCalls(
      "<|tool_call_start|>[memory.remember(text='la clave es (abc) [x]', "
      "confidence=0.9), memory.recall(query=\"wifi\")]<|tool_call_end|>");
  REQUIRE(bracketed.size() == 2);
  CHECK(bracketed[0].arguments["text"].asString() == "la clave es (abc) [x]");
  CHECK(bracketed[1].name == "memory.recall");
  CHECK(bracketed[1].arguments["query"].asString() == "wifi");

  const auto bare = LfmAdapter::parseToolCalls(
      "<|tool_call_start|>app.open(screen='agenda')<|tool_call_end|>");
  REQUIRE(bare.size() == 1);
  CHECK(bare[0].name == "app.open");
  CHECK(bare[0].arguments["screen"].asString() == "agenda");

  CHECK(LfmAdapter::parseToolCalls("[risas] claro que sí").empty());
}

TEST_CASE("a tool the model calls again in a later hop runs once")
{
  ProbeLog log;
  ToolRegistry registry;
  registry.registerTool(probe("probe.save", log));
  ScriptedEngine script;
  script.replies = {std::string(kCall), std::string(kCall), "Ya lo guardé."};
  LfmAdapter adapter({.engine = script.engine(), .registry = registry, .router = nullptr});

  std::vector<ChatMessage> history{{.role = "system", .content = "persona"},
                                   {.role = "user",
                                    .content = "recuerda que mi hermana viene los domingos"},
                                   {.role = "system", .content = "(Tono: cálido y corto.)"}};
  const auto output =
      adapter.chatWithTools(loopInput({registry.find("probe.save")}), history);

  CHECK(log.calls.size() == 1);
  CHECK(log.calls.front().context.utterance == "recuerda que mi hermana viene los domingos");
  CHECK(log.calls.front().context.sessionId == "voice-7-1");
  CHECK(output.reply == "Ya lo guardé.");
  REQUIRE(script.requests.size() == 3);
  CHECK(script.requests[0].toolCallsAllowed);
  CHECK_FALSE(script.requests[2].toolCallsAllowed);
  CHECK(script.requests[2].messages.front().content.find("List of tools") !=
        std::string::npos);
}

TEST_CASE("a call the turn does not offer is never run and never spoken")
{
  ProbeLog offeredLog;
  ProbeLog hiddenLog;
  ToolRegistry registry;
  registry.registerTool(probe("probe.save", offeredLog));
  registry.registerTool(probe("probe.hidden", hiddenLog));
  ScriptedEngine script;
  script.replies = {"<|tool_call_start|>[probe.hidden(text='x')]<|tool_call_end|>",
                    "Hola, ¿qué tal?"};
  LfmAdapter adapter({.engine = script.engine(), .registry = registry, .router = nullptr});

  std::vector<ChatMessage> history{{.role = "user", .content = "hola"}};
  std::string spoken;
  const TokenCallback onToken = [&spoken](const std::string& token, bool) { spoken += token; };
  const auto output = adapter.chatWithToolsStream(
      {.input = loopInput({registry.find("probe.save")}), .history = history, .onToken = onToken});

  CHECK(hiddenLog.calls.empty());
  CHECK(offeredLog.calls.empty());
  CHECK(spoken == "Hola, ¿qué tal?");
  CHECK(spoken.find("tool_call") == std::string::npos);
  CHECK(output.reply == "Hola, ¿qué tal?");
}

TEST_CASE("the router's call is shown to the model before the tool's answer")
{
  ProbeLog log;
  ToolRegistry registry;
  registry.registerTool(probe("memory.remember", log));
  PhraseCatalog catalog;
  catalog.build();
  const SilentClassifier classifier;
  const IntentRouter router({.catalog = catalog, .model = classifier, .recurrent = {}});
  ScriptedEngine script;
  script.replies = {"Listo, lo recordaré."};
  LfmAdapter adapter({.engine = script.engine(), .registry = registry, .router = &router});

  std::vector<ChatMessage> history{{.role = "system", .content = "persona"},
                                   {.role = "user",
                                    .content = "recuerda que mi hermana viene los domingos\n"
                                               "(Tono: cálido y corto.)"}};
  const auto output =
      adapter.chatWithTools(loopInput({registry.find("memory.remember")}), history);

  REQUIRE(log.calls.size() == 1);
  CHECK(log.calls.front().arguments["text"].asString() ==
        "recuerda que mi hermana viene los domingos");
  CHECK(log.calls.front().context.decided);
  CHECK(output.reply == "Listo, lo recordaré.");
  REQUIRE(script.requests.size() == 1);
  const auto& messages = script.requests.front().messages;
  REQUIRE(messages.size() == 4);
  CHECK(messages[2].role == "assistant");
  CHECK(messages[2].content.starts_with("<|tool_call_start|>[memory.remember("));
  CHECK(messages[3].role == "tool");
  CHECK(messages[3].content == "Guardado.");
  CHECK(messages.front().content.find("List of tools") != std::string::npos);
  CHECK_FALSE(script.requests.front().toolCallsAllowed);
}

TEST_CASE("a prefill-only turn primes the prompt a real turn starts from and runs nothing")
{
  ProbeLog log;
  ToolRegistry registry;
  registry.registerTool(probe("memory.remember", log));
  PhraseCatalog catalog;
  catalog.build();
  const SilentClassifier classifier;
  const IntentRouter router({.catalog = catalog, .model = classifier, .recurrent = {}});
  ScriptedEngine script;
  LfmAdapter adapter({.engine = script.engine(), .registry = registry, .router = &router});

  auto input = loopInput({registry.find("memory.remember")});
  input.prefillOnly = true;
  std::vector<ChatMessage> primed{{.role = "system", .content = "persona"},
                                  {.role = "assistant", .content = "Hola, soy Argus."}};
  std::string spoken;
  const TokenCallback onToken = [&spoken](const std::string& token, bool) { spoken += token; };
  adapter.chatWithToolsStream({.input = input, .history = primed, .onToken = onToken});
  REQUIRE(script.requests.size() == 1);
  CHECK(script.requests.front().prefillOnly);
  CHECK(log.calls.empty());

  input.prefillOnly = false;
  script.replies = {"Hola."};
  std::vector<ChatMessage> turn = primed;
  turn.push_back({.role = "user", .content = "hola"});
  adapter.chatWithTools(input, turn);
  REQUIRE(script.requests.size() == 2);
  CHECK_FALSE(script.requests[1].prefillOnly);
  const auto& primedMessages = script.requests[0].messages;
  const auto& turnMessages = script.requests[1].messages;
  REQUIRE(turnMessages.size() == primedMessages.size() + 1);
  for (std::size_t i = 0; i < primedMessages.size(); ++i) {
    CHECK(turnMessages[i].role == primedMessages[i].role);
    CHECK(turnMessages[i].content == primedMessages[i].content);
  }
}

TEST_CASE("a routed call the tool refuses falls back to the model with every tool")
{
  ToolRegistry registry;
  int runs = 0;
  registry.registerTool({.name = "memory.remember",
                         .description = "probe",
                         .arguments = {{.name = "text",
                                        .type = "string",
                                        .required = false,
                                        .enumValues = {},
                                        .description = ""}},
                         .accessTable = TableName::Memory,
                         .accessPermission = RolePermission::Create,
                         .handler = [&runs](const tools::ToolCall&) {
                           ++runs;
                           tools::ToolResult result;
                           result.output = "No pude guardar eso.";
                           return result;
                         }});
  PhraseCatalog catalog;
  catalog.build();
  const SilentClassifier classifier;
  const IntentRouter router({.catalog = catalog, .model = classifier, .recurrent = {}});
  ScriptedEngine script;
  script.replies = {"Claro, enciendo la luz de la cocina."};
  LfmAdapter adapter({.engine = script.engine(), .registry = registry, .router = &router});

  std::vector<ChatMessage> history{{.role = "user", .content = "recuerda que el wifi se cae cada semana"}};
  const auto output =
      adapter.chatWithTools(loopInput({registry.find("memory.remember")}), history);
  CHECK(runs == 1);
  REQUIRE(script.requests.size() == 1);
  CHECK(script.requests.front().toolCallsAllowed);
  CHECK(script.requests.front().messages.back().role == "user");
  CHECK(output.reply == "Claro, enciendo la luz de la cocina.");
}

TEST_CASE("explicit app commands become app calls and questions do not")
{
  const auto mode = commandOf("Pon la vigilancia en modo noche.");
  CHECK(mode.name == "app.set_guard_mode");
  CHECK(mode.arguments["mode"].asString() == "night");
  CHECK(commandOf("Activa el modo fuera, me voy").arguments["mode"].asString() == "away");
  CHECK(commandOf("set the guard mode to armed").arguments["mode"].asString() == "armed");
  CHECK_FALSE(appCommandFor("¿En qué modo está la vigilancia?").has_value());
  CHECK_FALSE(appCommandFor("Me voy a dormir").has_value());
  CHECK(commandOf("con la vigilancia en modo noche.").arguments["mode"].asString() == "night");
  CHECK(commandOf("Modo fuera.").arguments["mode"].asString() == "away");
  CHECK_FALSE(appCommandFor("está la vigilancia en modo noche").has_value());
  CHECK_FALSE(appCommandFor("anoche la vigilancia en modo noche saltó dos veces por el gato").has_value());
  CHECK_FALSE(mode.arguments.isMember("environment"));
  CHECK_FALSE(commandOf("Activa el modo fuera, me voy").arguments.isMember("environment"));
  const auto restaurant = commandOf("Pon el restaurante en modo armado, por favor");
  CHECK(restaurant.arguments["mode"].asString() == "armed");
  CHECK(restaurant.arguments["environment"].asString() == "restaurante");
  const auto cottage = commandOf("pon la vigilancia de la casa de campo en modo noche");
  CHECK(cottage.arguments["mode"].asString() == "night");
  CHECK(cottage.arguments["environment"].asString() == "casa campo");
  CHECK(commandOf("pon la casa en modo noche").arguments["environment"].asString() == "casa");
  CHECK_FALSE(commandOf("pon la vigilancia en casa").arguments.isMember("environment"));
  CHECK(commandOf("set the office guard mode to away").arguments["environment"].asString() ==
        "office");

  const auto garage = commandOf("Muéstrame la cámara del garaje, por favor.");
  CHECK(garage.name == "app.show_camera");
  CHECK(garage.arguments["camera"].asString() == "garaje");
  CHECK(commandOf("show me the garage camera").arguments["camera"].asString() == "garage");
  CHECK(commandOf("quiero ver la cámara 3").arguments["camera"].asString() == "3");
  CHECK(commandOf("enséñame la cámara").arguments["camera"].asString().empty());
  CHECK(commandOf("checa la cámara 4").arguments["camera"].asString() == "4");
  CHECK_FALSE(appCommandFor("quiero comprar una cámara nueva").has_value());
  CHECK_FALSE(appCommandFor("¿qué se ve en la cámara del patio?").has_value());

  const auto agenda = commandOf("Abre la agenda");
  CHECK(agenda.name == "app.open");
  CHECK(agenda.arguments["screen"].asString() == "agenda");
  CHECK(commandOf("abre las cámaras").arguments["screen"].asString() == "cameras");
  CHECK_FALSE(appCommandFor("hola, ¿cómo estás?").has_value());
}

TEST_CASE("an app command runs before the model when the call offers app tools")
{
  ToolRegistry registry;
  for (auto& descriptor : appToolDescriptors())
    registry.registerTool(std::move(descriptor));
  ScriptedEngine script;
  script.replies = {"Listo, modo noche activado."};
  LfmAdapter adapter({.engine = script.engine(), .registry = registry, .router = nullptr});

  std::vector<std::string> actions;
  auto input = loopInput({registry.find("app.set_guard_mode"), registry.find("app.show_camera")});
  input.role = UserRole::Owner;
  input.context.emitAction = [&actions](const std::string& name, const Json::Value&) {
    actions.push_back(name);
  };
  std::vector<ChatMessage> history{{.role = "user", .content = "Pon la vigilancia en modo noche."}};
  const auto output = adapter.chatWithTools(input, history);
  REQUIRE(actions.size() == 1);
  CHECK(actions.front() == "app.set_guard_mode");
  REQUIRE(script.requests.size() == 1);
  CHECK_FALSE(script.requests.front().toolCallsAllowed);
  CHECK(output.reply == "Listo, modo noche activado.");

  std::vector<ChatMessage> withoutApp{{.role = "user", .content = "Pon la vigilancia en modo noche."}};
  script.replies = {"No puedo cambiarla desde aquí."};
  adapter.chatWithTools(loopInput({}), withoutApp);
  CHECK(actions.size() == 1);
}

namespace
{
struct AppTurn
{
  ToolRegistry registry;
  std::vector<std::string> actions;
  ScriptedEngine script;
  std::string spoken;

  AppTurn()
  {
    for (auto& descriptor : appToolDescriptors())
      registry.registerTool(std::move(descriptor));
  }

  ToolChatOutput run(const std::string& utterance)
  {
    LfmAdapter adapter({.engine = script.engine(), .registry = registry, .router = nullptr});
    auto input = loopInput({registry.find("app.set_guard_mode"), registry.find("app.show_camera")});
    input.role = UserRole::Owner;
    input.context.emitAction = [this](const std::string& name, const Json::Value&) {
      actions.push_back(name);
    };
    std::vector<ChatMessage> history{{.role = "user", .content = utterance}};
    const TokenCallback onToken = [this](const std::string& token, bool) { spoken += token; };
    return adapter.chatWithToolsStream({.input = input, .history = history, .onToken = onToken});
  }
};
}

TEST_CASE("a reply that claims an app action no tool ran is never spoken and the model is asked again")
{
  AppTurn turn;
  turn.script.replies = {"Cambié la vigilancia a modo noche.",
                         "<|tool_call_start|>[app.set_guard_mode(mode='night')]<|tool_call_end|>",
                         "Listo, la vigilancia está en modo noche."};
  const auto output = turn.run("oye la vigilancia esta noche que esté atenta a todo por favor");
  REQUIRE(turn.actions.size() == 1);
  CHECK(turn.actions.front() == "app.set_guard_mode");
  CHECK(turn.spoken == "Listo, la vigilancia está en modo noche.");
  CHECK(turn.spoken.find("Cambié") == std::string::npos);
  REQUIRE(turn.script.requests.size() == 3);
  CHECK(turn.script.requests[1].messages.back().role == "system");
  CHECK(output.reply == "Listo, la vigilancia está en modo noche.");
}

TEST_CASE("a claim that survives the second ask becomes an honest question")
{
  AppTurn turn;
  turn.script.replies = {"Cambié la vigilancia a modo noche.", "Ya está activado el modo noche."};
  const auto output = turn.run("oye la vigilancia esta noche que esté atenta a todo por favor");
  CHECK(turn.actions.empty());
  CHECK(turn.spoken == "Todavía no lo he hecho. ¿Quieres que lo haga?");
  CHECK(output.reply == turn.spoken);
}

TEST_CASE("a question about the app or a turn without app words streams as before")
{
  AppTurn question;
  question.script.replies = {"Está en modo noche, lo cambiaste anoche."};
  question.run("¿en qué modo está la vigilancia?");
  CHECK(question.actions.empty());
  CHECK(question.spoken == "Está en modo noche, lo cambiaste anoche.");
  REQUIRE(question.script.requests.size() == 1);

  AppTurn chat;
  chat.script.replies = {"¡Qué bien! Ya está hecho entonces."};
  chat.run("mi hermana ya llegó a casa");
  CHECK(chat.spoken == "¡Qué bien! Ya está hecho entonces.");
  CHECK(chat.script.requests.size() == 1);
}
