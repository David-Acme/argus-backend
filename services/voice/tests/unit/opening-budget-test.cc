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
