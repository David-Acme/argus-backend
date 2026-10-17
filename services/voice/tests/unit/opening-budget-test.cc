#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <feature/voice/opening-budget.hxx>

#include <string>
#include <vector>

namespace
{
std::vector<std::string> kOpener()
{
  return {
    "Hola, Administrador.",
    "Te llamo por algo urgente.",
    "Alguien de casa ha pulsado el botón de pánico.",
    "Puede necesitar ayuda ahora mismo.",
  };
}

int chargedMs(const std::string& sentence)
{
  return static_cast<int>(utf8Length(sentence)) * 90;
}
}

TEST_CASE("the opener is split into sentences at . ! and ?, and the spaces are dropped")
{
  const auto parts = splitSentences("Hola, Administrador. ¿Estás bien? Sí!  Bien.");
  REQUIRE(parts.size() == 4);
  CHECK(parts[0] == "Hola, Administrador.");
  CHECK(parts[1] == "¿Estás bien?");
  CHECK(parts[2] == "Sí!");
  CHECK(parts[3] == "Bien.");
}

TEST_CASE("sentences are synthesized only while they fit the budget; the rest become later lines")
{
  std::vector<std::string> synthesized;
  OpeningBudgetInput input{
      .sentences = kOpener(),
      .budgetMs = kOpeningBudgetMs,
      .estimateMsPerChar = 90.0,
      .synthesizeMs = [&synthesized](const std::string& sentence) {
        synthesized.push_back(sentence);
        return chargedMs(sentence);
      }};
  const auto result = fitOpening(input);
  REQUIRE(result.spoken.size() == 1);
  CHECK(result.spoken[0] == "Hola, Administrador.");
  REQUIRE(result.later.size() == 3);
  CHECK(result.later[0] == "Te llamo por algo urgente.");
  CHECK(result.later[2] == "Puede necesitar ayuda ahora mismo.");
  CHECK(synthesized.size() == 1);
}

TEST_CASE("the first sentence is always spoken, even when it alone passes the budget")
{
  OpeningBudgetInput input{
      .sentences = {"Una frase muy larga que sola ya supera el presupuesto de cuatro segundos de audio.",
                    "Otra."},
      .budgetMs = 1000,
      .estimateMsPerChar = 90.0,
      .synthesizeMs = [](const std::string& sentence) { return static_cast<int>(utf8Length(sentence)) * 90; }};
  const auto result = fitOpening(input);
  REQUIRE(result.spoken.size() == 1);
  REQUIRE(result.later.size() == 1);
  CHECK(result.later[0] == "Otra.");
}

TEST_CASE("the measured rate replaces the estimate for the next sentence")
{
  OpeningBudgetInput input{
      .sentences = {"Hola.", "Adiós, ya te digo algo más largo ahora mismo."},
      .budgetMs = kOpeningBudgetMs,
      .estimateMsPerChar = 1.0,
      .synthesizeMs = [](const std::string& sentence) { return static_cast<int>(utf8Length(sentence)) * 90; }};
  const auto result = fitOpening(input);
  CHECK(result.spoken.size() == 1);
  REQUIRE(result.later.size() == 1);
  CHECK(result.msPerChar == doctest::Approx(90.0));
}

TEST_CASE("a panic opener is one sentence that carries the reason, so the budget never defers it")
{
  const auto sentences = splitSentences("Hola, Administrador, Tom pulsó el botón de pánico en Casa.");
  REQUIRE(sentences.size() == 1);
  OpeningBudgetInput input{
      .sentences = sentences,
      .budgetMs = kOpeningBudgetMs,
      .estimateMsPerChar = 90.0,
      .synthesizeMs = [](const std::string& sentence) { return chargedMs(sentence); }};
  const auto result = fitOpening(input);
  REQUIRE(result.spoken.size() == 1);
  CHECK(result.spoken[0].find("botón de pánico") != std::string::npos);
  CHECK(result.later.empty());
}

struct KindOpener
{
  const char* name;
  std::string line;
  const char* reason;
};

std::vector<KindOpener> kindOpeners()
{
  return {
      {.name = "a critical guard episode",
       .line = "Hola, Laura, te llamo por algo urgente de la vigilancia: hay una "
               "persona desconocida en Patio, de noche. Le estoy avisando por el "
               "altavoz. ¿Quieres que te muestre la cámara?",
       .reason = "hay una persona desconocida"},
      {.name = "a guard episode while nobody is home",
       .line = "Hola, Laura, te llamo por la vigilancia: hay una persona "
               "desconocida en Patio, de noche. Le estoy avisando por el altavoz. "
               "¿Quieres que te muestre la cámara?",
       .reason = "hay una persona desconocida"},
      {.name = "a guard episode that got worse",
       .line = "Hola, Laura, te llamo porque la situación ha empeorado: hay una "
               "persona desconocida en Patio, de noche. Le estoy avisando por el "
               "altavoz. ¿Quieres que te muestre la cámara?",
       .reason = "hay una persona desconocida"},
      {.name = "a discreet guard episode",
       .line = "Hola, Laura, te aviso en voz baja: hay una persona desconocida en "
               "Patio. No abras y quédate dentro. ¿Quieres ver la cámara?",
       .reason = "hay una persona desconocida"},
      {.name = "a panic button",
       .line = "Hola, Laura, Tom pulsó el botón de pánico en Casa.",
       .reason = "Tom pulsó el botón de pánico"},
      {.name = "a silent alert",
       .line = "Hola, Laura, Tom ha enviado una alerta silenciosa en Casa; puede "
               "estar bajo amenaza, no le llames.",
       .reason = "Tom ha enviado una alerta silenciosa"},
      {.name = "a camera that stopped seeing",
       .line = "Hola, Laura, la cámara Patio ha dejado de ver; puede estar "
               "tapada o desconectada.",
       .reason = "la cámara Patio ha dejado de ver"},
      {.name = "someone arriving",
       .line = "Hola, Laura, te aviso de que ha llegado Ana a Casa.",
       .reason = "ha llegado Ana"},
      {.name = "an agenda event starting now",
       .line = "Hola, Laura, te llamo por tu agenda: ahora empieza «Dentista».",
       .reason = "«Dentista»"},
      {.name = "an agenda reminder",
       .line = "Hola, Laura, te recuerdo: Llamar al dentista.",
       .reason = "Llamar al dentista"},
      {.name = "an assistant call with a topic",
       .line = "Hola, Laura, me pediste que te llamara para recordarte: llamar "
               "al dentista.",
       .reason = "llamar al dentista"},
      {.name = "an assistant call without a topic",
       .line = "Hola, Laura, ¿en qué te ayudo?",
       .reason = "¿en qué te ayudo?"},
  };
}

TEST_CASE("a call opening's first chunk carries the reason: a greeting is never spoken alone while the reason waits")
{
  for (const KindOpener& opener : kindOpeners()) {
    CAPTURE(opener.name);
    const auto sentences = splitSentences(opener.line);
    REQUIRE_FALSE(sentences.empty());
    CHECK(sentences.front().find(opener.reason) != std::string::npos);
    OpeningBudgetInput input{
        .sentences = sentences,
        .budgetMs = kOpeningBudgetMs,
        .estimateMsPerChar = 90.0,
        .synthesizeMs = [](const std::string& sentence) { return chargedMs(sentence); }};
    const auto result = fitOpening(input);
    REQUIRE_FALSE(result.spoken.empty());
    CHECK(result.spoken.front() != "Hola, Laura.");
    CHECK(result.spoken.front() == sentences.front());
    CHECK(result.spoken.front().find(opener.reason) != std::string::npos);
    for (const auto& later : result.later)
      CHECK(later.find(opener.reason) == std::string::npos);
  }
}

TEST_CASE("a reason-first sentence that alone exceeds the budget is still spoken first, whole, and the rest waits")
{
  const std::string opening =
      "Hola, Laura, me pediste que te llamara para recordarte: comprar el pan y "
      "llamar al dentista antes de que cierre la consulta de la calle mayor. "
      "Además, tienes una cita a las cinco.";
  const auto sentences = splitSentences(opening);
  REQUIRE(sentences.size() == 2);
  std::vector<std::string> synthesized;
  OpeningBudgetInput input{
      .sentences = sentences,
      .budgetMs = 1000,
      .estimateMsPerChar = 90.0,
      .synthesizeMs = [&synthesized](const std::string& sentence) {
        synthesized.push_back(sentence);
        return chargedMs(sentence);
      }};
  const auto result = fitOpening(input);
  REQUIRE(result.spoken.size() == 1);
  CHECK(result.spoken.front() == sentences.front());
  CHECK(result.spoken.front().find("llamar al dentista") != std::string::npos);
  REQUIRE(result.later.size() == 1);
  CHECK(result.later.front() == "Además, tienes una cita a las cinco.");
  CHECK(synthesized.size() == 1);
}
