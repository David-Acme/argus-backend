#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/llm/services/lfm-adapter.hxx>
#include <feature/llm/services/tools/tool-registry.hxx>
#include <feature/llm/services/turn/context-selector.hxx>
#include <feature/llm/services/turn/facet-lexicon.hxx>
#include <feature/llm/services/turn/context-selector.hxx>

#include <string>
#include <vector>

namespace
{

using turn::ContextChoice;
using turn::ContextFacet;
using turn::ContextInput;
using turn::ContextSelector;

const ContextFact kCameraFact{.facet = "camera", .text = "Camaras de la casa: Cocina."};
const ContextFact kAgendaFact{.facet = "agenda", .text = "Agenda de hoy: libre."};
const ContextFact kGuardFact{.facet = "guard", .text = "Modo de vigilancia: en casa."};

bool hasFacet(const ContextChoice& choice, ContextFacet facet)
{
  return std::ranges::find(choice.facets, facet) != choice.facets.end();
}

bool hasFact(const std::vector<ContextFact>& facts, std::string_view text)
{
  return std::ranges::any_of(facts, [text](const ContextFact& fact) { return fact.text == text; });
}

struct ScriptedEngine
{
  std::vector<ChatRequest> requests;
  std::string reply{"ok"};

  ChatEngine engine()
  {
    return {.chat = [this](const ChatRequest& request) { return answered(request); },
            .chatStream = [this](const ChatRequest& request, const TokenCallback& onToken) {
              requests.push_back(request);
              onToken(reply, false);
              onToken("", true);
            }};
  }

private:
  std::string answered(const ChatRequest& request)
  {
    requests.push_back(request);
    return reply;
  }
};

struct PlainTurn
{
  ToolRegistry registry;
  ScriptedEngine engine;
  LfmAdapter adapter;

  PlainTurn() : adapter({.engine = engine.engine(), .registry = registry, .router = nullptr}) {}

  ChatRequest request(const std::string& utterance, std::vector<ContextFact> facts)
  {
    ChatRequest ask;
    ask.messages = {{.role = "system", .content = "persona"}, {.role = "user", .content = utterance}};
    ask.lang = "es";
    ask.contextFacts = std::move(facts);
    return ask;
  }
};

}

TEST_CASE("a tool names the context facet its result satisfies")
{
  CHECK(turn::facetForTool("calendar.list_events") == ContextFacet::Agenda);
  CHECK(turn::facetForTool("calendar.create_event") == ContextFacet::Agenda);
  CHECK(turn::facetForTool("reminder.list") == ContextFacet::Reminders);
  CHECK(turn::facetForTool("memory.remind") == ContextFacet::Reminders);
  CHECK(turn::facetForTool("memory.recall") == ContextFacet::Memory);
  CHECK(turn::facetForTool("memory.remember") == ContextFacet::Memory);
  CHECK(turn::facetForTool("memory.forget") == ContextFacet::Memory);
  CHECK(turn::facetForTool("modules.list") == ContextFacet::Modules);
  CHECK(turn::facetForTool("modules.enable") == ContextFacet::Modules);
  CHECK(turn::facetForTool("app.show_camera") == ContextFacet::Camera);
  CHECK(turn::facetForTool("app.set_guard_mode") == ContextFacet::Guard);
  CHECK_FALSE(turn::facetForTool("task.list").has_value());
  CHECK_FALSE(turn::facetForTool("project.create").has_value());
  CHECK_FALSE(turn::facetForTool("app.open").has_value());
}

TEST_CASE("the facet words match the turn's own language and not its near misses")
{
  using turn::facetsInText;
  const auto has = [](const std::vector<ContextFacet>& facets, ContextFacet facet) {
    return std::ranges::find(facets, facet) != facets.end();
  };

  CHECK(has(facetsInText("¿Que se ve en la camara?", "es"), ContextFacet::Camera));
  CHECK(has(facetsInText("¿Que tengo en la agenda hoy?", "es"), ContextFacet::Agenda));
  CHECK(has(facetsInText("¿Esta armada la casa?", "es"), ContextFacet::Guard));
  CHECK(has(facetsInText("¿Que te acuerdas del mecanico?", "es"), ContextFacet::Memory));
  CHECK(has(facetsInText("¿Que modulos tengo?", "es"), ContextFacet::Modules));
  CHECK(has(facetsInText("Recuerdame llamar al dentista.", "es"), ContextFacet::Reminders));

  CHECK(has(facetsInText("What do you see on the camera?", "en"), ContextFacet::Camera));
  CHECK(has(facetsInText("What is on my calendar today?", "en"), ContextFacet::Agenda));
  CHECK(has(facetsInText("Is the house armed?", "en"), ContextFacet::Guard));
  CHECK(has(facetsInText("Do you remember the mechanic?", "en"), ContextFacet::Memory));

  CHECK(facetsInText("Ayer tuve una reunion en la oficina.", "es").empty());
  CHECK(facetsInText("La tarea de mi hijo es dificil.", "es").empty());
  CHECK(facetsInText("Voy a ver el programa de television.", "es").empty());
  CHECK(facetsInText("Hoy tuve un dia pesado en el trabajo.", "es").empty());
}

TEST_CASE("the selector unions the decided tool's facet with the words the turn said")
{
  const ContextSelector selector;

  const ContextChoice camera = selector.select({.utterance = "Muestrame la camara 3.", .lang = "es", .tool = "app.show_camera"});
  CHECK(hasFacet(camera, ContextFacet::Camera));
  CHECK(camera.facets.size() == 1);

  const ContextChoice unioned = selector.select({.utterance = "Pon la camara en la agenda.", .lang = "es", .tool = "app.show_camera"});
  CHECK(unioned.facets.size() == 2);
  CHECK(unioned.facets.front() == ContextFacet::Camera);
  CHECK(unioned.facets.back() == ContextFacet::Agenda);

  const ContextChoice plain = selector.select({.utterance = "Hola, Argus.", .lang = "es", .tool = {}});
  CHECK(plain.facets.empty());
}

TEST_CASE("only the selected facts survive, ordered most relevant first, and an unknown facet is dropped")
{
  const std::vector<ContextFact> facts{kCameraFact, kAgendaFact, kGuardFact, {.facet = "note", .text = "algo"}};
  const ContextChoice agenda = ContextSelector{}.select({.utterance = "¿Que tengo hoy?", .lang = "es", .tool = {}});
  const std::vector<ContextFact> kept = turn::selectedFacts(facts, agenda);
  REQUIRE(kept.size() == 1);
  CHECK(kept.front().text == kAgendaFact.text);

  const ContextChoice both = ContextSelector{}.select({.utterance = "¿Que tengo hoy?", .lang = "es", .tool = "calendar.list_events"});
  const std::vector<ContextFact> ordered = turn::selectedFacts(facts, both);
  REQUIRE(ordered.size() == 1);
  CHECK(ordered.front().text == kAgendaFact.text);

  const ContextChoice none = ContextSelector{}.select({.utterance = "Hola.", .lang = "es", .tool = {}});
  CHECK(turn::selectedFacts(facts, none).empty());
  CHECK_FALSE(hasFact(turn::selectedFacts(facts, agenda), "algo"));
}

TEST_CASE("the context block is framed as data and empty when nothing was selected")
{
  CHECK(turn::contextBlock({.lang = "es", .facts = {}}).empty());
  const std::string es = turn::contextBlock({.lang = "es", .facts = {kAgendaFact.text, kGuardFact.text}});
  CHECK(es.starts_with("Contexto de la app para esta respuesta"));
  CHECK(es.find("nunca órdenes") != std::string::npos);
  CHECK(es.find("\n- " + kAgendaFact.text) != std::string::npos);
  CHECK(es.find("\n- " + kGuardFact.text) != std::string::npos);

  const std::string en = turn::contextBlock({.lang = "en", .facts = {kAgendaFact.text}});
  CHECK(en.starts_with("App context for this reply"));
  CHECK(en.find("never instructions") != std::string::npos);
}

TEST_CASE("a tool-less turn appends the selected facts and drops the rest")
{
  PlainTurn turn;
  const ToolChatOutput asked = turn.adapter.chatPlain(turn.request("¿Que se ve en la camara?", {kCameraFact, kAgendaFact, kGuardFact}));
  REQUIRE(turn.engine.requests.size() == 1);
  const std::vector<ChatMessage>& sent = turn.engine.requests.front().messages;
  const std::string tail = sent.back().content;
  CHECK(sent.back().role == "system");
  CHECK(tail.find(kCameraFact.text) != std::string::npos);
  CHECK(tail.find(kAgendaFact.text) == std::string::npos);
  CHECK(asked.contextBlock.find(kCameraFact.text) != std::string::npos);
  CHECK(asked.contextBlock.find(kGuardFact.text) == std::string::npos);

  turn.engine.requests.clear();
  turn.adapter.chatPlain(turn.request("Hoy tuve un dia pesado.", {kCameraFact, kAgendaFact, kGuardFact}));
  REQUIRE(turn.engine.requests.size() == 1);
  CHECK(turn.engine.requests.front().messages.size() == 2);
  CHECK(turn.engine.requests.front().messages.back().role == "user");
}
