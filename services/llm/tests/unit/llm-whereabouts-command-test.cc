#include <doctest/doctest.h>

#include <feature/llm/services/tools/whereabouts-command.hxx>
#include <feature/llm/services/turn/deciders.hxx>

#include "tool-stubs.hxx"

#include <memory>
#include <string>
#include <utility>
#include <vector>
#include <test-support/value-of.hxx>

namespace
{
tools::ToolCall whereOf(const std::string& utterance)
{
  auto command = whereaboutsCommandFor(utterance);
  REQUIRE_MESSAGE(command.has_value(), utterance);
  return std::move(command).value_or(tools::ToolCall{});
}

std::string personOf(const std::string& utterance)
{
  return whereOf(utterance).arguments.get("person", "").asString();
}

std::string roomOf(const std::string& utterance)
{
  return whereOf(utterance).arguments.get("room", "").asString();
}
}

TEST_CASE("where a named member is, asked in Spanish and English, names the person")
{
  CHECK(whereOf("¿dónde está David?").name == "guard.where_is");
  CHECK(personOf("¿dónde está David?") == "david");
  CHECK(personOf("dónde está Ana ahora") == "ana");
  CHECK(personOf("¿Dónde anda Luis?") == "luis");
  CHECK(personOf("¿en qué parte de la casa está Marta?") == "marta");
  CHECK(personOf("¿en qué cuarto está Ana Sofía?") == "ana sofia");
  CHECK(personOf("where is Ana?") == "ana");
  CHECK(personOf("Where's David right now?") == "david");
  CHECK(personOf("¿está David en casa?") == "david");
  CHECK(personOf("is Ana home?") == "ana");
  CHECK(personOf("is Ana at home") == "ana");
}

TEST_CASE("who is in a room names the room, and who is home asks for the household")
{
  CHECK(roomOf("¿quién está en la sala?") == "sala");
  CHECK(roomOf("¿quiénes están en la cocina ahora?") == "cocina");
  CHECK(roomOf("¿hay alguien en el garaje?") == "garaje");
  CHECK(roomOf("who is in the kitchen?") == "kitchen");
  CHECK(roomOf("is anyone in the living room right now") == "living room");
  const auto home = whereOf("¿quién está en casa?");
  CHECK(home.arguments.getMemberNames().empty());
  CHECK(whereOf("who's home?").arguments.getMemberNames().empty());
}

TEST_CASE("a camera, an object, a pronoun or a longer question is never read as a person")
{
  for (const char* utterance :
       {"¿dónde está la cámara del patio?", "¿dónde está la cámara?", "¿Dónde está el hielo?",
        "¿Exactamente dónde está la piscina?", "where is the prime meridian", "where is the spare key hidden",
        "where is justin bieber's home base", "Where is he?", "¿dónde está?", "¿dónde está mi celular?",
        "¿dónde está mamá?", "¿Hay alguien en el patio ahora mismo?", "is anyone in the backyard right now",
        "who's at the door right now", "¿quién está en la puerta ahora mismo?", "who is in the kitchen camera",
        "Is a leap year every four years, or are there exceptions I keep forgetting?",
        "esta casa siempre está fría en invierno", "is it raining outside", "¿qué tengo el día 5?",
        "A mi hermana le ofrecieron liderar el proyecto, pero prefirió quedarse donde está.",
        "David está en casa"}) {
    INFO(utterance);
    CHECK_FALSE(whereaboutsCommandFor(utterance).has_value());
  }
}

TEST_CASE("the rules tier routes where someone is only when the tool is offered")
{
  const tools::ToolHandle handle = std::make_shared<const tools::ToolDescriptor>(
      tool_stubs::stub({.name = "guard.where_is",
                        .capability = "presence.read",
                        .handler = [](const tools::ToolCall&) { return tool_stubs::okResult("David está en la sala."); },
                        .module = "surveillance"}));
  const std::vector<tools::ToolHandle> offered{handle};
  const std::vector<tools::ToolHandle> none;
  const ModuleSnapshot modules;
  const turn::RuleDecider decider;

  const auto picked = valueOf(decider.decide(
      {.utterance = "¿dónde está David?", .lang = "es", .offered = offered, .modules = modules, .previousAssistant = {}}));
  CHECK(picked.tool == "guard.where_is");
  CHECK(picked.exact);
  CHECK(picked.source == "whereabouts command");
  CHECK(picked.arguments["person"].asString() == "david");

  CHECK_FALSE(decider
                  .decide({.utterance = "¿dónde está David?",
                           .lang = "es",
                           .offered = none,
                           .modules = modules,
                           .previousAssistant = {}})
                  .has_value());
}
