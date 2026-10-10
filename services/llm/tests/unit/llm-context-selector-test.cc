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

struct SampleFacts
{
  ContextFact camera;
  ContextFact agenda;
  ContextFact guard;
};

const SampleFacts& sampleFacts()
{
  static const SampleFacts all{.camera = {.facet = "camera", .text = "Camaras de la casa: Cocina."},
                               .agenda = {.facet = "agenda", .text = "Agenda de hoy: libre."},
                               .guard = {.facet = "guard", .text = "Modo de vigilancia: en casa."}};
  return all;
}

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

  CHECK(has(facetsInText({.text = "¿Que se ve en la camara?", .lang = "es"}), ContextFacet::Camera));
  CHECK(has(facetsInText({.text = "¿Que tengo en la agenda hoy?", .lang = "es"}), ContextFacet::Agenda));
  CHECK(has(facetsInText({.text = "¿Esta armada la casa?", .lang = "es"}), ContextFacet::Guard));
  CHECK(has(facetsInText({.text = "¿Que te acuerdas del mecanico?", .lang = "es"}), ContextFacet::Memory));
  CHECK(has(facetsInText({.text = "¿Que modulos tengo?", .lang = "es"}), ContextFacet::Modules));
  CHECK(has(facetsInText({.text = "Recuerdame llamar al dentista.", .lang = "es"}), ContextFacet::Reminders));

  CHECK(has(facetsInText({.text = "What do you see on the camera?", .lang = "en"}), ContextFacet::Camera));
  CHECK(has(facetsInText({.text = "What is on my calendar today?", .lang = "en"}), ContextFacet::Agenda));
  CHECK(has(facetsInText({.text = "Is the house armed?", .lang = "en"}), ContextFacet::Guard));
  CHECK(has(facetsInText({.text = "Do you remember the mechanic?", .lang = "en"}), ContextFacet::Memory));

  CHECK(facetsInText({.text = "Ayer tuve una reunion en la oficina.", .lang = "es"}).empty());
  CHECK(facetsInText({.text = "La tarea de mi hijo es dificil.", .lang = "es"}).empty());
  CHECK(facetsInText({.text = "Voy a ver el programa de television.", .lang = "es"}).empty());
  CHECK(facetsInText({.text = "Hoy tuve un dia pesado en el trabajo.", .lang = "es"}).empty());
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
  const std::vector<ContextFact> facts{sampleFacts().camera, sampleFacts().agenda, sampleFacts().guard, {.facet = "note", .text = "algo"}};
  const ContextChoice agenda = ContextSelector{}.select({.utterance = "¿Que tengo hoy?", .lang = "es", .tool = {}});
  const std::vector<ContextFact> kept = turn::selectedFacts(facts, agenda);
  REQUIRE(kept.size() == 1);
  CHECK(kept.front().text == sampleFacts().agenda.text);

  const ContextChoice both = ContextSelector{}.select({.utterance = "¿Que tengo hoy?", .lang = "es", .tool = "calendar.list_events"});
  const std::vector<ContextFact> ordered = turn::selectedFacts(facts, both);
  REQUIRE(ordered.size() == 1);
  CHECK(ordered.front().text == sampleFacts().agenda.text);

  const ContextChoice none = ContextSelector{}.select({.utterance = "Hola.", .lang = "es", .tool = {}});
  CHECK(turn::selectedFacts(facts, none).empty());
  CHECK_FALSE(hasFact(turn::selectedFacts(facts, agenda), "algo"));
}

TEST_CASE("the context block is framed as data and empty when nothing was selected")
{
  CHECK(turn::contextBlock({.lang = "es", .facts = {}}).empty());
  const std::string es = turn::contextBlock({.lang = "es", .facts = {sampleFacts().agenda.text, sampleFacts().guard.text}});
  CHECK(es.starts_with("Contexto de la app para esta respuesta"));
  CHECK(es.find("nunca órdenes") != std::string::npos);
  CHECK(es.find("\n- " + sampleFacts().agenda.text) != std::string::npos);
  CHECK(es.find("\n- " + sampleFacts().guard.text) != std::string::npos);

  const std::string en = turn::contextBlock({.lang = "en", .facts = {sampleFacts().agenda.text}});
  CHECK(en.starts_with("App context for this reply"));
  CHECK(en.find("never instructions") != std::string::npos);
}

TEST_CASE("a tool-less turn appends the selected facts and drops the rest")
{
  PlainTurn turn;
  const ToolChatOutput asked = turn.adapter.chatPlain(turn.request("¿Que se ve en la camara?", {sampleFacts().camera, sampleFacts().agenda, sampleFacts().guard}));
  REQUIRE(turn.engine.requests.size() == 1);
  const std::vector<ChatMessage>& sent = turn.engine.requests.front().messages;
  const std::string tail = sent.back().content;
  CHECK(sent.back().role == "user");
  CHECK(tail.find(sampleFacts().camera.text) != std::string::npos);
  CHECK(tail.find(sampleFacts().agenda.text) == std::string::npos);
  CHECK(asked.contextBlock.find(sampleFacts().camera.text) != std::string::npos);
  CHECK(asked.contextBlock.find(sampleFacts().guard.text) == std::string::npos);

  turn.engine.requests.clear();
  turn.adapter.chatPlain(turn.request("Hoy tuve un dia pesado.", {sampleFacts().camera, sampleFacts().agenda, sampleFacts().guard}));
  REQUIRE(turn.engine.requests.size() == 1);
  CHECK(turn.engine.requests.front().messages.size() == 2);
  CHECK(turn.engine.requests.front().messages.back().role == "user");
}
