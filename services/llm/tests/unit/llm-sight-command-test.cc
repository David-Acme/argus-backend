#include <doctest/doctest.h>

#include <feature/llm/services/tools/sight-command.hxx>

#include <string>
#include <utility>

namespace
{
tools::ToolCall sightOf(const std::string& utterance)
{
  auto command = sightCommandFor(utterance);
  REQUIRE(command.has_value());
  return std::move(command).value_or(tools::ToolCall{});
}
}

TEST_CASE("turning a camera to a place is a preset by name, never the model's choice")
{
  const auto door = sightOf("mira hacia la puerta");
  CHECK(door.name == "camera.goto_preset");
  CHECK(door.arguments["preset"].asString() == "puerta");
  CHECK(door.arguments["camera"].asString().empty());
  const auto living = sightOf("gira la cámara de la sala a la entrada");
  CHECK(living.name == "camera.goto_preset");
  CHECK(living.arguments["camera"].asString() == "sala");
  CHECK(living.arguments["preset"].asString() == "entrada");
  CHECK_FALSE(sightCommandFor("mira a mi hermana, ya llegó").has_value());
  CHECK_FALSE(sightCommandFor("turn on the lights").has_value());
}

TEST_CASE("a direction moves the camera and the home words bring it back")
{
  const auto left = sightOf("mueve la cámara del patio a la izquierda");
  CHECK(left.name == "camera.look");
  CHECK(left.arguments["direction"].asString() == "left");
  CHECK(left.arguments["camera"].asString() == "patio");
  const auto home = sightOf("gira la cámara del garaje a su posición de vigilancia");
  CHECK(home.name == "camera.look");
  CHECK(home.arguments["direction"].asString() == "home");
}

TEST_CASE("the assistant hears a maintenance window opening and closing")
{
  const auto start = sightOf("voy a limpiar la cámara del patio");
  CHECK(start.name == "guard.maintenance_start");
  CHECK(start.arguments["camera"].asString() == "patio");
  const auto timed = sightOf("voy a darle mantenimiento a la cámara de la entrada por 30 minutos");
  CHECK(timed.name == "guard.maintenance_start");
  CHECK(timed.arguments["minutes"].asInt() == 30);
  CHECK(sightOf("ya terminé con la cámara").name == "guard.maintenance_end");
  CHECK(sightOf("I'm done with the camera maintenance").name == "guard.maintenance_end");
  CHECK_FALSE(sightCommandFor("ya terminé la tarea del colegio").has_value());
}

TEST_CASE("asking why Argus warned is a status read")
{
  CHECK(sightOf("¿Por qué me avisaste?").name == "guard.maintenance_status");
  CHECK(sightOf("¿Quién movió la cámara?").name == "guard.maintenance_status");
  CHECK_FALSE(sightCommandFor("¿Qué hora es?").has_value());
}
