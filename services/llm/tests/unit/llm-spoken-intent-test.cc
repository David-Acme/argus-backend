#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/llm/services/tools/spoken-intent.hxx>

#include <string>

namespace
{
ModuleFlag productivity()
{
  ModuleFlag flag{.id = "productivity", .enabled = false};
  flag.name = {.es = "Productividad", .en = "Productivity"};
  return flag;
}

ModuleFlag surveillance()
{
  ModuleFlag flag{.id = "surveillance", .enabled = false};
  flag.name = {.es = "Vigilancia y cámaras", .en = "Surveillance and cameras"};
  return flag;
}
}

TEST_CASE("a short clear yes is an affirmation, in either language and with the accents the speech engine drops")
{
  for (const char* yes : {"sí", "Sí.", "si, por favor", "claro", "Dale", "ok", "vale, hazlo", "confirmo", "sí, cancélala",
                          "yes", "Yes, please", "sure", "go ahead", "yeah do it"})
    CHECK_MESSAGE(spoken_intent::affirms(yes), yes);
}

TEST_CASE("anything that hedges, refuses or wanders is not an affirmation")
{
  for (const char* other : {"", "no", "no, mejor no", "sí pero primero cuéntame el clima", "sí, no lo hagas", "cancela",
                            "espera un momento", "qué hora es", "yes but wait", "not now", "don't do it",
                            "sí claro cuando termine de comer te aviso para que lo hagas ahora mismo",
                            "gracias", "tal vez"})
  {
    INFO(other);
    CHECK_FALSE(spoken_intent::affirms(other));
  }
}

TEST_CASE("naming a module with an enabling verb is an explicit ask to enable it")
{
  const ModuleFlag module = productivity();
  for (const char* ask : {"activa productividad", "Activa el módulo de Productividad", "enable productivity",
                          "instala productivity por favor", "turn on productivity"})
    CHECK_MESSAGE(spoken_intent::asksToEnable({.utterance = ask, .module = module}), ask);
  for (const char* other : {"qué es productividad", "activa la agenda", "no actives productividad",
                            "productividad", "enable surveillance"})
  {
    INFO(other);
    CHECK_FALSE(spoken_intent::asksToEnable({.utterance = other, .module = module}));
  }
}

TEST_CASE("a module whose name has several words is named only when all of them are heard")
{
  const ModuleFlag module = surveillance();
  CHECK(spoken_intent::asksToEnable({.utterance = "activa vigilancia y cámaras", .module = module}));
  CHECK(spoken_intent::asksToEnable({.utterance = "activa surveillance", .module = module}));
  CHECK_FALSE(spoken_intent::asksToEnable({.utterance = "activa vigilancia", .module = module}));
}

TEST_CASE("asking to request a module from the owner needs a requesting verb")
{
  const ModuleFlag module = productivity();
  CHECK(spoken_intent::asksToRequest({.utterance = "pídele al dueño que active productividad", .module = module}));
  CHECK(spoken_intent::asksToRequest({.utterance = "ask for productivity", .module = module}));
  CHECK_FALSE(spoken_intent::asksToRequest({.utterance = "activa productividad", .module = module}));
  CHECK_FALSE(spoken_intent::asksToRequest({.utterance = "no pidas productividad", .module = module}));
}
