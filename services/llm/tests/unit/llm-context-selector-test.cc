#include <doctest/doctest.h>

#include <feature/llm/services/lfm-adapter.hxx>
#include <feature/llm/services/tools/tool-registry.hxx>
#include <feature/llm/services/turn/context-selector.hxx>
#include <feature/llm/services/turn/facet-lexicon.hxx>

#include <json/reader.h>
#include <json/value.h>
#include <text/name-match.hxx>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <sstream>
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

std::vector<std::string> facetNames(const std::vector<ContextFacet>& facets)
{
  std::vector<std::string> names;
  names.reserve(facets.size());
  for (const ContextFacet facet : facets)
    names.emplace_back(turn::contextFacetToString(facet));
  return names;
}

bool namesFacet(const std::vector<ContextFacet>& facets, std::string_view name)
{
  const std::vector<std::string> names = facetNames(facets);
  return std::ranges::find(names, name) != names.end();
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

  PlainTurn() : adapter({.engine = engine.engine(), .registry = registry}) {}

  ChatRequest request(const std::string& utterance, std::vector<ContextFact> facts)
  {
    ChatRequest ask;
    ask.messages = {{.role = "system", .content = "persona"}, {.role = "user", .content = utterance}};
    ask.lang = "es";
    ask.contextFacts = std::move(facts);
    return ask;
  }
};

struct PackTurn
{
  std::string id;
  std::string lang;
  std::string user;
};

std::vector<std::string> packPaths()
{
  std::vector<std::string> paths;
  const char* value = std::getenv("ARGUS_TEST_CONTEXT_PACK");
  if (value == nullptr)
    return paths;
  std::istringstream stream(value);
  std::string path;
  while (std::getline(stream, path, ':'))
    if (!path.empty())
      paths.push_back(path);
  return paths;
}

std::vector<PackTurn> packTurns()
{
  std::vector<PackTurn> turns;
  for (const std::string& path : packPaths()) {
    std::ifstream in(path);
    std::string line;
    while (std::getline(in, line)) {
      if (line.empty())
        continue;
      Json::Value root;
      std::string errors;
      Json::CharReaderBuilder builder;
      std::istringstream stream(line);
      if (!Json::parseFromStream(builder, stream, &root, &errors))
        continue;
      for (const auto& node : root["turns"])
        turns.push_back({.id = root.get("id", "").asString(),
                         .lang = root.get("lang", "es").asString(),
                         .user = node.get("user", "").asString()});
    }
  }
  return turns;
}

}

TEST_CASE("a tool names the context facet its result satisfies")
{
  CHECK(turn::facetForTool("calendar.list_events") == ContextFacet::Agenda);
  CHECK(turn::facetForTool("calendar.create_event") == ContextFacet::Agenda);
  CHECK(turn::facetForTool("task.list") == ContextFacet::Tasks);
  CHECK(turn::facetForTool("task.create") == ContextFacet::Tasks);
  CHECK(turn::facetForTool("project.list") == ContextFacet::Projects);
  CHECK(turn::facetForTool("project.create") == ContextFacet::Projects);
  CHECK(turn::facetForTool("reminder.list") == ContextFacet::Reminders);
  CHECK(turn::facetForTool("memory.remind") == ContextFacet::Reminders);
  CHECK(turn::facetForTool("memory.recall") == ContextFacet::Memory);
  CHECK(turn::facetForTool("memory.remember") == ContextFacet::Memory);
  CHECK(turn::facetForTool("memory.forget") == ContextFacet::Memory);
  CHECK(turn::facetForTool("modules.list") == ContextFacet::Modules);
  CHECK(turn::facetForTool("modules.enable") == ContextFacet::Modules);
  CHECK(turn::facetForTool("app.show_camera") == ContextFacet::Camera);
  CHECK(turn::facetForTool("app.set_guard_mode") == ContextFacet::Guard);
  CHECK_FALSE(turn::facetForTool("app.open").has_value());
  CHECK_FALSE(turn::facetForTool("").has_value());
}

TEST_CASE("every facet has a name and an unknown name reads as no facet")
{
  const std::vector<std::string> names{"camera", "agenda", "guard", "reminders", "memory",
                                       "modules", "people", "tasks", "projects"};
  REQUIRE(static_cast<std::size_t>(ContextFacet::Count) == names.size());
  for (const std::string& name : names)
    CHECK(turn::contextFacetToString(turn::contextFacetFromString(name)) == name);
  CHECK(turn::contextFacetFromString("note") == ContextFacet::Count);
  CHECK(turn::contextFacetFromString("") == ContextFacet::Count);
}

TEST_CASE("the facet words match the turn's own language and not its near misses")
{
  using turn::facetsInText;

  CHECK(namesFacet(facetsInText({.text = "¿Que se ve en la camara?", .lang = "es"}), "camera"));
  CHECK(namesFacet(facetsInText({.text = "¿Que tengo en la agenda hoy?", .lang = "es"}), "agenda"));
  CHECK(namesFacet(facetsInText({.text = "¿Esta armada la casa?", .lang = "es"}), "guard"));
  CHECK(namesFacet(facetsInText({.text = "¿Que te acuerdas del mecanico?", .lang = "es"}), "memory"));
  CHECK(namesFacet(facetsInText({.text = "¿Que modulos tengo?", .lang = "es"}), "modules"));
  CHECK(namesFacet(facetsInText({.text = "Recuerdame llamar al dentista.", .lang = "es"}), "reminders"));

  CHECK(namesFacet(facetsInText({.text = "What do you see on the camera?", .lang = "en"}), "camera"));
  CHECK(namesFacet(facetsInText({.text = "What is on my calendar today?", .lang = "en"}), "agenda"));
  CHECK(namesFacet(facetsInText({.text = "Is the house armed?", .lang = "en"}), "guard"));
  CHECK(namesFacet(facetsInText({.text = "Do you remember the mechanic?", .lang = "en"}), "memory"));

  CHECK(namesFacet(facetsInText({.text = "¿quiénes están en casa ahora?", .lang = "es"}), "people"));
  CHECK(namesFacet(facetsInText({.text = "¿dónde está mi hijo?", .lang = "es"}), "people"));
  CHECK(namesFacet(facetsInText({.text = "Where is my son?", .lang = "en"}), "people"));
  CHECK(namesFacet(facetsInText({.text = "Who is home right now?", .lang = "en"}), "people"));

  CHECK(namesFacet(facetsInText({.text = "¿qué tengo pendiente?", .lang = "es"}), "tasks"));
  CHECK(namesFacet(facetsInText({.text = "¿hasta cuándo tengo para pintar la sala?", .lang = "es"}), "tasks"));
  CHECK(namesFacet(facetsInText({.text = "When is the living room painting due?", .lang = "en"}), "tasks"));
  CHECK(namesFacet(facetsInText({.text = "When is the present task due?", .lang = "en"}), "tasks"));

  CHECK(namesFacet(facetsInText({.text = "¿en qué va el proyecto del baño?", .lang = "es"}), "projects"));
  CHECK(namesFacet(facetsInText({.text = "¿qué proyectos tengo abiertos?", .lang = "es"}), "projects"));
  CHECK(namesFacet(facetsInText({.text = "How is the project going?", .lang = "en"}), "projects"));

  CHECK(namesFacet(facetsInText({.text = "¿cuándo es la cita del dentista?", .lang = "es"}), "agenda"));
  CHECK(namesFacet(facetsInText({.text = "¿y a qué hora era la reunión?", .lang = "es"}), "agenda"));
  CHECK(namesFacet(facetsInText({.text = "Cuando es la reunion del colegio", .lang = "es"}), "agenda"));

  CHECK(namesFacet(facetsInText({.text = "¿a qué hora llegó Marta?", .lang = "es"}), "guard"));
  CHECK(namesFacet(facetsInText({.text = "¿vino sola?", .lang = "es"}), "guard"));
  CHECK(namesFacet(facetsInText({.text = "¿vino con su pata?", .lang = "es"}), "guard"));
  CHECK(namesFacet(facetsInText({.text = "¿llegó alguien más después?", .lang = "es"}), "guard"));
  CHECK(namesFacet(facetsInText({.text = "What time did Marta arrive?", .lang = "en"}), "guard"));
  CHECK(namesFacet(facetsInText({.text = "Was she alone?", .lang = "en"}), "guard"));
  CHECK(namesFacet(facetsInText({.text = "¿quién fue?", .lang = "es"}), "guard"));
  CHECK(namesFacet(facetsInText({.text = "¿qué hago?", .lang = "es"}), "guard"));
  CHECK(namesFacet(facetsInText({.text = "llama a la policía", .lang = "es"}), "guard"));
  CHECK(namesFacet(facetsInText({.text = "What should I do?", .lang = "en"}), "guard"));
  CHECK(namesFacet(facetsInText({.text = "¿qué está haciendo?", .lang = "es"}), "guard"));
  CHECK(namesFacet(facetsInText({.text = "What are they doing?", .lang = "en"}), "guard"));

  CHECK(namesFacet(facetsInText({.text = "¿a qué hora tengo que pagar la luz?", .lang = "es"}), "reminders"));
  CHECK(namesFacet(facetsInText({.text = "¿cuándo toca pagar la luz?", .lang = "es"}), "reminders"));
  CHECK(namesFacet(facetsInText({.text = "What time do I have to pay the electric bill?", .lang = "en"}),
                   "reminders"));
  CHECK(namesFacet(facetsInText({.text = "¿y qué día?", .lang = "es"}), "reminders"));

  CHECK(facetsInText({.text = "Voy a ver el programa de television.", .lang = "es"}).empty());
  CHECK(facetsInText({.text = "Hoy tuve un dia pesado en el trabajo.", .lang = "es"}).empty());
  CHECK(facetsInText({.text = "I like long walks on the beach.", .lang = "en"}).empty());
  CHECK(facetsInText({.text = "Cuentame un chiste.", .lang = "es"}).empty());
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

  const ContextChoice task = selector.select({.utterance = "¿Que tareas tengo?", .lang = "es", .tool = "task.list"});
  CHECK(task.facets.size() == 1);
  CHECK(task.facets.front() == ContextFacet::Tasks);

  const ContextChoice plain = selector.select({.utterance = "Hola, Argus.", .lang = "es", .tool = {}});
  CHECK(plain.facets.empty());
}

TEST_CASE("only the selected facts survive, ordered most relevant first, and an unknown facet is dropped")
{
  const std::vector<ContextFact> facts{sampleFacts().camera, sampleFacts().agenda, sampleFacts().guard,
                                       {.facet = "people", .text = "Miembros de la casa: Maria."},
                                       {.facet = "tasks", .text = "Tareas pendientes: comprar el pan."},
                                       {.facet = "note", .text = "algo"}};
  const ContextChoice agenda = ContextSelector{}.select({.utterance = "¿Que tengo hoy?", .lang = "es", .tool = {}});
  const std::vector<ContextFact> kept = turn::selectedFacts(facts, agenda);
  REQUIRE(kept.size() == 1);
  CHECK(kept.front().text == sampleFacts().agenda.text);

  const ContextChoice both = ContextSelector{}.select({.utterance = "¿Que tengo hoy?", .lang = "es", .tool = "calendar.list_events"});
  const std::vector<ContextFact> ordered = turn::selectedFacts(facts, both);
  REQUIRE(ordered.size() == 1);
  CHECK(ordered.front().text == sampleFacts().agenda.text);

  const ContextChoice people = ContextSelector{}.select({.utterance = "¿Donde esta mi hijo?", .lang = "es", .tool = {}});
  CHECK(hasFact(turn::selectedFacts(facts, people), "Miembros de la casa: Maria."));
  CHECK_FALSE(hasFact(turn::selectedFacts(facts, people), sampleFacts().camera.text));

  const ContextChoice none = ContextSelector{}.select({.utterance = "Hola.", .lang = "es", .tool = {}});
  CHECK(turn::selectedFacts(facts, none).empty());
  CHECK_FALSE(hasFact(turn::selectedFacts(facts, agenda), "algo"));
}

TEST_CASE("the facts the turn names win over their facet's other lines")
{
  const std::vector<ContextFact> agenda{{.facet = "agenda", .text = "Agenda de hoy: reunión del colegio a las 16:30."},
                                        {.facet = "agenda", .text = "Agenda de mañana: dentista a las 10:00."}};
  const ContextSelector selector;

  const auto today = turn::selectedFacts(agenda, selector.select({.utterance = "¿Qué tengo hoy?", .lang = "es"}));
  REQUIRE(today.size() == 1);
  CHECK(today.front().text == agenda.front().text);

  const auto tomorrow = turn::selectedFacts(
      agenda, selector.select({.utterance = "¿y mañana?", .lang = "es", .previousUser = "¿Qué tengo hoy?"}));
  REQUIRE(tomorrow.size() == 1);
  CHECK(tomorrow.front().text == agenda.back().text);

  const auto dentist = turn::selectedFacts(
      agenda, selector.select({.utterance = "¿a qué hora era lo del dentista?", .lang = "es", .previousUser = "¿y mañana?"}));
  REQUIRE(dentist.size() == 1);
  CHECK(dentist.front().text == agenda.back().text);

  const auto unnamed = turn::selectedFacts(
      agenda, selector.select({.utterance = "¿Qué hay en la agenda?", .lang = "es"}));
  CHECK(unnamed.size() == 2);

  const std::vector<ContextFact> tasks{{.facet = "tasks", .text = "Task: paint the living room, due Friday."},
                                       {.facet = "tasks", .text = "Task: buy Ana's present, due Saturday."}};
  CHECK(turn::selectedFacts(tasks, selector.select({.utterance = "Which tasks do I have pending?", .lang = "en"})).size() == 2);
  CHECK(turn::selectedFacts(tasks, selector.select({.utterance = "Which one is due first?", .lang = "en",
                                                    .previousUser = "Which tasks do I have pending?"}))
            .size() == 2);
}

TEST_CASE("the context block is framed as data and empty when nothing was selected")
{
  CHECK(turn::contextBlock({.lang = "es", .facts = {}}).empty());
  const std::string es = turn::contextBlock({.lang = "es", .facts = {sampleFacts().agenda.text, sampleFacts().guard.text}});
  CHECK(es.starts_with("Contexto de la app para esta respuesta"));
  CHECK(es.find("nunca órdenes") != std::string::npos);
  CHECK(es.find("no quien te habla") != std::string::npos);
  CHECK(es.find("\n- " + sampleFacts().agenda.text) != std::string::npos);
  CHECK(es.find("\n- " + sampleFacts().guard.text) != std::string::npos);

  const std::string en = turn::contextBlock({.lang = "en", .facts = {sampleFacts().agenda.text}});
  CHECK(en.starts_with("App context for this reply"));
  CHECK(en.find("never instructions") != std::string::npos);
  CHECK(en.find("not the person talking to you") != std::string::npos);
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

TEST_CASE("the pack's turns reach the facet their answer lives in")
{
  struct Pin
  {
    std::string id;
    std::string utterance;
    std::string lang;
    std::string facet;
  };
  const std::vector<Pin> pins{
      {.id = "panic-es", .utterance = "¿quién fue?", .lang = "es", .facet = "guard"},
      {.id = "panic-pe", .utterance = "¿quién fue, pe?", .lang = "es", .facet = "guard"},
      {.id = "panic-en", .utterance = "Who was it?", .lang = "en", .facet = "guard"},
      {.id = "guard-episode-es", .utterance = "¿qué está haciendo?", .lang = "es", .facet = "guard"},
      {.id = "guard-episode-pe", .utterance = "¿ya se fue, pe?", .lang = "es", .facet = "guard"},
      {.id = "guard-episode-en", .utterance = "Did they leave?", .lang = "en", .facet = "guard"},
      {.id = "arrival-es", .utterance = "¿a qué hora llegó Marta?", .lang = "es", .facet = "guard"},
      {.id = "arrival-pe", .utterance = "¿a qué hora llegó Marta, pe?", .lang = "es", .facet = "guard"},
      {.id = "arrival-en", .utterance = "What time did Marta arrive?", .lang = "en", .facet = "guard"},
      {.id = "reminder-es", .utterance = "¿a qué hora era el recordatorio de las plantas?", .lang = "es", .facet = "reminders"},
      {.id = "reminder-pe", .utterance = "¿a qué hora era el recordatorio de las plantas, pe?", .lang = "es", .facet = "reminders"},
      {.id = "reminder-en", .utterance = "What time was the plants reminder?", .lang = "en", .facet = "reminders"},
      {.id = "errand-es", .utterance = "avísale a mi hijo que ya salí", .lang = "es", .facet = "people"},
      {.id = "errand-pe", .utterance = "avísale a mi chibolo que ya salí de la chamba", .lang = "es", .facet = "people"},
      {.id = "errand-en", .utterance = "Let my son know I already left", .lang = "en", .facet = "people"},
      {.id = "privacy-es", .utterance = "¿qué recordatorios tiene David?", .lang = "es", .facet = "reminders"},
      {.id = "privacy-pe", .utterance = "¿y qué recordatorios tiene el dueño?", .lang = "es", .facet = "reminders"},
      {.id = "privacy-en", .utterance = "What reminders does David have?", .lang = "en", .facet = "reminders"},
      {.id = "module-off-es", .utterance = "¿qué ves en la cámara de la entrada?", .lang = "es", .facet = "camera"},
      {.id = "module-off-pe", .utterance = "¿qué se ve en la cámara de la entrada, pe?", .lang = "es", .facet = "camera"},
      {.id = "module-off-en", .utterance = "What can you see on the front door camera?", .lang = "en", .facet = "camera"},
      {.id = "role-inactive-es", .utterance = "¿tengo algún recordatorio pendiente?", .lang = "es", .facet = "reminders"},
      {.id = "role-inactive-pe", .utterance = "¿tengo algún recordatorio pendiente, pe?", .lang = "es", .facet = "reminders"},
      {.id = "role-inactive-en", .utterance = "Do I have any reminders pending?", .lang = "en", .facet = "reminders"},
      {.id = "duress-es", .utterance = "¿cómo está todo en casa?", .lang = "es", .facet = "guard"},
      {.id = "duress-pe", .utterance = "¿cómo está todo en el jato?", .lang = "es", .facet = "guard"},
      {.id = "duress-en", .utterance = "How is everything at home?", .lang = "en", .facet = "guard"},
      {.id = "relay-panic-es", .utterance = "¿dónde está?", .lang = "es", .facet = "guard"},
      {.id = "relay-panic-pe", .utterance = "¿y dónde está?", .lang = "es", .facet = "guard"},
      {.id = "relay-panic-en", .utterance = "Where is he?", .lang = "en", .facet = "guard"},
      {.id = "barge-in-es", .utterance = "¿y mañana qué hay?", .lang = "es", .facet = "agenda"},
      {.id = "barge-in-pe", .utterance = "¿y mañana qué hay?", .lang = "es", .facet = "agenda"},
      {.id = "barge-in-en", .utterance = "And what about tomorrow?", .lang = "en", .facet = "agenda"},
      {.id = "a-digression-es", .utterance = "¿y a qué hora era la reunión?", .lang = "es", .facet = "agenda"},
      {.id = "a-digression-pe", .utterance = "¿y a qué hora era la reunión?", .lang = "es", .facet = "agenda"},
      {.id = "a-digression-en", .utterance = "And what time was the meeting?", .lang = "en", .facet = "agenda"},
      {.id = "a-followup-es", .utterance = "¿y a qué hora llegó?", .lang = "es", .facet = "guard"},
      {.id = "a-followup-pe", .utterance = "¿y a qué hora llegó?", .lang = "es", .facet = "guard"},
      {.id = "a-followup-en", .utterance = "And what time was that?", .lang = "en", .facet = "guard"},
      {.id = "a-register-es", .utterance = "¿qué tengo pendiente?", .lang = "es", .facet = "tasks"},
      {.id = "a-register-pe", .utterance = "¿qué tengo pendiente?", .lang = "es", .facet = "tasks"},
      {.id = "a-register-en", .utterance = "What do I have pending?", .lang = "en", .facet = "tasks"},
      {.id = "b-select-es", .utterance = "¿cuándo es la cita del dentista?", .lang = "es", .facet = "agenda"},
      {.id = "b-select-pe", .utterance = "¿cuándo es la cita del dentista, pe?", .lang = "es", .facet = "agenda"},
      {.id = "b-select-en", .utterance = "When is the dentist appointment?", .lang = "en", .facet = "agenda"},
      {.id = "b-recency-es", .utterance = "¿cuándo vence lo de pintar la sala?", .lang = "es", .facet = "tasks"},
      {.id = "b-recency-pe", .utterance = "¿hasta cuándo tengo para pintar la sala?", .lang = "es", .facet = "tasks"},
      {.id = "b-recency-en", .utterance = "When is the living room painting due?", .lang = "en", .facet = "tasks"},
      {.id = "b-conflict-es", .utterance = "¿está funcionando la cámara de la entrada?", .lang = "es", .facet = "camera"},
      {.id = "b-conflict-pe", .utterance = "¿está funcionando la cámara de la entrada?", .lang = "es", .facet = "camera"},
      {.id = "b-conflict-en", .utterance = "Is the front door camera working?", .lang = "en", .facet = "camera"},
      {.id = "b-numbers-es", .utterance = "¿a qué hora tengo que pagar la luz?", .lang = "es", .facet = "reminders"},
      {.id = "b-numbers-pe", .utterance = "¿a qué hora tengo que pagar la luz, pe?", .lang = "es", .facet = "reminders"},
      {.id = "b-numbers-en", .utterance = "What time do I have to pay the electric bill?", .lang = "en", .facet = "reminders"},
      {.id = "b-entity-es", .utterance = "¿dónde está mi hijo?", .lang = "es", .facet = "people"},
      {.id = "b-entity-pe", .utterance = "¿dónde está mi chibolo?", .lang = "es", .facet = "people"},
      {.id = "b-entity-en", .utterance = "Where is my son?", .lang = "en", .facet = "people"},
      {.id = "b-partial-es", .utterance = "¿cuándo vence la tarea del regalo?", .lang = "es", .facet = "tasks"},
      {.id = "b-partial-pe", .utterance = "¿hasta cuándo tengo para comprar el regalo?", .lang = "es", .facet = "tasks"},
      {.id = "b-partial-en", .utterance = "When is the present task due?", .lang = "en", .facet = "tasks"},
      {.id = "b-long-es", .utterance = "¿a qué hora es la revisión del carro?", .lang = "es", .facet = "agenda"},
      {.id = "b-long-pe", .utterance = "¿a qué hora es la revisión del carro, pe?", .lang = "es", .facet = "agenda"},
      {.id = "b-long-en", .utterance = "What time is the car service?", .lang = "en", .facet = "agenda"}};

  const std::vector<PackTurn> pack = packTurns();
  REQUIRE_MESSAGE(!pack.empty(), "ARGUS_TEST_CONTEXT_PACK is not set");
  for (const Pin& pin : pins) {
    const bool known = std::ranges::any_of(pack, [&pin](const PackTurn& turn) {
      return turn.id == pin.id && turn.user == pin.utterance;
    });
    CHECK_MESSAGE(known, pin.id << " carries no turn " << pin.utterance);
    CHECK_MESSAGE(namesFacet(turn::facetsInText({.text = pin.utterance, .lang = pin.lang}), pin.facet),
                  pin.id << " reads " << pin.utterance << " as no " << pin.facet);
  }
  std::vector<std::string> ids;
  for (const PackTurn& turn : pack)
    if (std::ranges::find(ids, turn.id) == ids.end())
      ids.push_back(turn.id);
  for (const std::string& id : ids) {
    const bool pinned = std::ranges::any_of(pins, [&id](const Pin& pin) { return pin.id == id; });
    CHECK_MESSAGE(pinned, id << " has no pinned turn");
  }
}
