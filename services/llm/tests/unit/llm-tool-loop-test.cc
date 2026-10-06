#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/llm/services/tools/reply-claims.hxx>
#include <feature/intent/services/intent-contracts.hxx>
#include <feature/intent/services/intent-router.hxx>
#include <feature/llm/services/lfm-adapter.hxx>
#include <feature/llm/services/tools/app-command.hxx>
#include <feature/llm/services/tools/tool-registry.hxx>
#include <phrase/phrase-catalog.hxx>
#include "tool-stubs.hxx"

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
  return tool_stubs::stub({.name = name,
                           .capability = "memory.manage",
                           .handler = [&log](const tools::ToolCall& call) {
                             log.calls.push_back(call);
                             return tool_stubs::okResult("Guardado.");
                           }});
}

class SilentClassifier final : public intent::IIntentClassifier
{
public:
  [[nodiscard]] bool isLoaded() const override { return false; }
  [[nodiscard]] std::vector<intent::IntentHit> score(const std::string&) const override { return {}; }
};

ToolChatInput loopInput(const std::vector<tools::ToolHandle>& offered)
{
  ToolChatInput input;
  input.systemPrompt = "Eres Argus.";
  input.tools = offered;
  input.audience = {.role = UserRole::Resident, .modules = {}};
  input.context = {.userId = 7,
                   .role = UserRole::Resident,
                   .lang = "es",
                   .sessionId = "voice-7-1",
                   .channel = "tool_result",
                   .utterance = {},
                   .decided = false,
                   .turn = 0,
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
  registry.registerTool(tool_stubs::stub({.name = "memory.remember",
                                          .capability = "memory.manage",
                                          .handler = [&runs](const tools::ToolCall&) {
                                            ++runs;
                                            tools::ToolResult result;
                                            result.output = "No pude guardar eso.";
                                            return result;
                                          }}));
  PhraseCatalog catalog;
  catalog.build();
  const SilentClassifier classifier;
  const IntentRouter router({.catalog = catalog, .model = classifier, .recurrent = {}});
  ScriptedEngine script;
  script.replies = {"Lo siento, no pude guardar eso."};
  LfmAdapter adapter({.engine = script.engine(), .registry = registry, .router = &router});

  std::vector<ChatMessage> history{{.role = "user", .content = "recuerda que el wifi se cae cada semana"}};
  const auto output =
      adapter.chatWithTools(loopInput({registry.find("memory.remember")}), history);
  CHECK(runs == 1);
  REQUIRE(script.requests.size() == 1);
  CHECK(script.requests.front().toolCallsAllowed);
  CHECK(script.requests.front().messages.back().role == "user");
  CHECK(output.reply == "Lo siento, no pude guardar eso.");
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
  registry.registerTool(tool_stubs::appAction({.name = "app.set_guard_mode", .capability = "guard.mode.set", .module = "surveillance"}));
  registry.registerTool(tool_stubs::appAction({.name = "app.show_camera", .capability = "camera.view", .module = "surveillance"}));
  ScriptedEngine script;
  script.replies = {"Listo, modo noche activado."};
  LfmAdapter adapter({.engine = script.engine(), .registry = registry, .router = nullptr});

  std::vector<std::string> actions;
  auto input = loopInput({registry.find("app.set_guard_mode"), registry.find("app.show_camera")});
  input.audience.role = UserRole::Owner;
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
    registry.registerTool(tool_stubs::appAction({.name = "app.set_guard_mode", .capability = "guard.mode.set", .module = "surveillance"}));
    registry.registerTool(tool_stubs::appAction({.name = "app.show_camera", .capability = "camera.view", .module = "surveillance"}));
  }

  ToolChatOutput run(const std::string& utterance)
  {
    LfmAdapter adapter({.engine = script.engine(), .registry = registry, .router = nullptr});
    auto input = loopInput({registry.find("app.set_guard_mode"), registry.find("app.show_camera")});
    input.audience.role = UserRole::Owner;
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
  CHECK(turn.spoken == "No pude hacerlo. ¿Lo intento de nuevo?");
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

namespace
{
constexpr std::string_view kScheduleCall =
    "<|tool_call_start|>[calendar.create_event(text='reunión con Andrea')]<|tool_call_end|>";

struct Behaviour
{
  bool ok{true};
  bool readOnly{false};
};

struct ClaimTurn
{
  ToolRegistry registry;
  ScriptedEngine script;
  int runs{0};
  std::string spoken;
  std::vector<std::string> pieces;
  std::string lang{"es"};

  explicit ClaimTurn(Behaviour behaviour = {})
  {
    auto descriptor = tool_stubs::stub({.name = "calendar.create_event",
                                        .capability = "agenda.write",
                                        .handler = [this, behaviour](const tools::ToolCall&) {
                                          ++runs;
                                          tools::ToolResult result;
                                          result.ok = behaviour.ok;
                                          result.output = behaviour.ok ? "Agendado." : "No pude agendar.";
                                          return result;
                                        },
                                        .module = "productivity"});
    descriptor.spec.annotations.readOnly = behaviour.readOnly;
    registry.registerTool(std::move(descriptor));
  }

  ToolChatInput input() const
  {
    auto loop = loopInput({registry.find("calendar.create_event")});
    loop.context.lang = lang;
    return loop;
  }

  ToolChatOutput sync(const std::string& utterance)
  {
    LfmAdapter adapter({.engine = script.engine(), .registry = registry, .router = nullptr});
    std::vector<ChatMessage> history{{.role = "user", .content = utterance}};
    return adapter.chatWithTools(input(), history);
  }

  ToolChatOutput stream(const std::string& utterance)
  {
    LfmAdapter adapter({.engine = script.engine(), .registry = registry, .router = nullptr});
    std::vector<ChatMessage> history{{.role = "user", .content = utterance}};
    const TokenCallback onToken = [this](const std::string& token, bool) {
      if (!token.empty())
        pieces.push_back(token);
      spoken += token;
    };
    return adapter.chatWithToolsStream({.input = input(), .history = history, .onToken = onToken});
  }
};

std::string honestSpanish()
{
  return reply_claims::honest("es");
}
}

TEST_CASE("a claim with no tool behind it is asked again once and then answered honestly")
{
  ClaimTurn turn;
  turn.script.replies = {"Creo una reunión con Andrea para el jueves a las 3:00 PM. Confirmado.",
                         "Listo, ya te la agendé."};
  const auto output = turn.sync("Agéndame una reunión con Andrea el jueves a las tres");
  CHECK(turn.runs == 0);
  CHECK(output.reply == honestSpanish());
  REQUIRE(turn.script.requests.size() == 2);
  CHECK(turn.script.requests[1].messages.back().role == "system");
  CHECK(turn.script.requests[1].messages.back().content == reply_claims::nudge("es"));
}

TEST_CASE("the second ask can become a real call, and what follows the tool is spoken as it is")
{
  ClaimTurn turn;
  turn.script.replies = {"Agendé la reunión con Andrea.", std::string(kScheduleCall), "Listo, quedó agendada con Andrea."};
  const auto output = turn.sync("Agéndame una reunión con Andrea el jueves a las tres");
  CHECK(turn.runs == 1);
  CHECK(output.reply == "Listo, quedó agendada con Andrea.");
  CHECK(turn.script.requests.size() == 3);
}

TEST_CASE("a tool that failed does not make a later claim true")
{
  ClaimTurn turn({.ok = false, .readOnly = false});
  turn.script.replies = {std::string(kScheduleCall), "Listo, ya lo agendé.", "Ya lo agendé pues."};
  const auto output = turn.sync("Agéndame una reunión con Andrea el jueves a las tres");
  CHECK(turn.runs == 1);
  CHECK(output.reply == honestSpanish());
  CHECK(output.executed.size() == 1);
}

TEST_CASE("a claim after a tool that worked is spoken, and so is a claim in English")
{
  ClaimTurn turn;
  turn.script.replies = {std::string(kScheduleCall), "Listo, ya lo agendé para el jueves."};
  const auto output = turn.sync("Agéndame una reunión con Andrea el jueves a las tres");
  CHECK(turn.runs == 1);
  CHECK(output.reply == "Listo, ya lo agendé para el jueves.");

  ClaimTurn english;
  english.lang = "en";
  english.script.replies = {std::string(kScheduleCall), "Done, I've scheduled it for Thursday."};
  const auto spoken = english.sync("Schedule a meeting with Andrea on Thursday at three");
  CHECK(spoken.reply == "Done, I've scheduled it for Thursday.");
}

TEST_CASE("a tool that only reads does not make a claim of writing true")
{
  ClaimTurn turn({.ok = true, .readOnly = true});
  turn.script.replies = {std::string(kScheduleCall), "Agendé la reunión con Andrea."};
  const auto output = turn.sync("¿Qué tengo mañana en la agenda?");
  CHECK(turn.runs == 1);
  CHECK(output.reply == honestSpanish());
}

TEST_CASE("a claim nobody asked for is answered honestly at once, and a plain reply is untouched")
{
  ClaimTurn claim;
  claim.script.replies = {"Guardé tu nota."};
  const auto output = claim.sync("gracias");
  CHECK(output.reply == honestSpanish());
  CHECK(claim.script.requests.size() == 1);

  ClaimTurn plain;
  plain.script.replies = {"Tienes tres eventos mañana."};
  CHECK(plain.sync("¿qué tengo mañana?").reply == "Tienes tres eventos mañana.");

  ClaimTurn english;
  english.lang = "en";
  english.script.replies = {"I have created the task.", "I've added it to your list."};
  CHECK(english.sync("Add a task to call the dentist").reply == reply_claims::honest("en"));
}

TEST_CASE("a streamed claim is cut at its sentence and what came before it was already spoken")
{
  ClaimTurn turn;
  turn.script.chunk = 4;
  turn.script.replies = {"Hola, Ana. Creo una reunión con Andrea para el jueves. Confirmado."};
  const auto output = turn.stream("gracias por todo");
  CHECK(turn.spoken == "Hola, Ana. " + honestSpanish());
  CHECK(output.reply == turn.spoken);
  REQUIRE_FALSE(turn.pieces.empty());
  CHECK(turn.pieces.front() == "Hola, Ana.");
  CHECK(turn.runs == 0);
}

TEST_CASE("a streamed reply with no claim arrives sentence by sentence, not all at the end")
{
  ClaimTurn turn;
  turn.script.chunk = 3;
  turn.script.replies = {"Claro que sí. Dime qué necesitas. Aquí estoy."};
  const auto output = turn.stream("hola Argus");
  CHECK(turn.spoken == "Claro que sí. Dime qué necesitas. Aquí estoy.");
  CHECK(output.reply == turn.spoken);
  CHECK(turn.pieces.size() == 3);
}

TEST_CASE("a streamed claim after a tool that worked is spoken whole")
{
  ClaimTurn turn;
  turn.script.chunk = 5;
  turn.script.replies = {std::string(kScheduleCall), "Listo, ya lo agendé para el jueves. Confirmado."};
  const auto output = turn.stream("Agéndame una reunión con Andrea el jueves a las tres");
  CHECK(turn.runs == 1);
  CHECK(turn.spoken == "Listo, ya lo agendé para el jueves. Confirmado.");
  CHECK(output.reply == turn.spoken);
}

TEST_CASE("a streamed turn that was asked for something holds the claim, asks again and speaks the real answer")
{
  ClaimTurn turn;
  turn.script.chunk = 6;
  turn.script.replies = {"Creo una reunión con Andrea para el jueves. Confirmado.", std::string(kScheduleCall),
                         "Listo, quedó agendada."};
  const auto output = turn.stream("Agéndame una reunión con Andrea el jueves a las tres");
  CHECK(turn.runs == 1);
  CHECK(turn.spoken == "Listo, quedó agendada.");
  CHECK(output.reply == "Listo, quedó agendada.");
  CHECK(turn.spoken.find("Creo") == std::string::npos);
  CHECK(turn.script.requests.size() == 3);
}

TEST_CASE("a streamed turn whose tool failed ends honestly and never speaks the claim")
{
  ClaimTurn turn({.ok = false, .readOnly = false});
  turn.script.chunk = 7;
  turn.script.replies = {std::string(kScheduleCall), "Listo, ya lo agendé.", "Ya lo agendé pues."};
  const auto output = turn.stream("Agéndame una reunión con Andrea el jueves a las tres");
  CHECK(turn.spoken == honestSpanish());
  CHECK(output.reply == honestSpanish());
}
