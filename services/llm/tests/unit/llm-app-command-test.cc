#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/llm/services/lfm-adapter.hxx>
#include <feature/llm/services/tools/app-command.hxx>

#include <utility>

namespace
{

tools::ToolCall commandOf(const std::string& utterance)
{
  auto command = appCommandFor(utterance);
  REQUIRE(command.has_value());
  return std::move(command).value_or(tools::ToolCall{});
}

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
