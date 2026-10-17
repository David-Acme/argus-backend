#pragma once

#include <text/secret-phrase.hxx>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace context_eval
{

inline constexpr std::string_view kDuressSalt = "duress-scenarios";
static_assert(kDuressSalt.size() == secret_phrase::kSaltBytes);

[[nodiscard]] std::optional<std::string> sealedDuressPhrase(std::string_view phrase);

struct ScenarioFact
{
  std::string facet;
  std::string text;
};

struct ScenarioTurn
{
  std::string user;
  std::vector<ScenarioFact> facts;
  std::vector<std::vector<std::string>> keep;
  std::vector<std::vector<std::string>> first;
  std::vector<std::string> never;
  std::vector<std::string> noRepeat;
  std::string act;
  bool refuse{false};
  bool moduleOff{false};
  bool conflict{false};
  bool partial{false};
  bool question{false};
  bool noQuestion{false};
  bool bargeIn{false};
  bool duressHold{false};
  int maxSentences{3};
  int maxWords{45};
};

struct ScenarioCall
{
  std::string trigger;
  std::string opening;
  std::vector<std::vector<std::string>> reason;
};

struct ScenarioDuress
{
  std::string phrase;
  int turn{0};
};

struct ScenarioCase
{
  std::string id;
  std::string scenario;
  std::string axis;
  std::string locale;
  std::string lang;
  std::string variant;
  std::string role;
  std::string route;
  std::string name;
  std::string person;
  std::vector<std::string> modulesOff;
  std::optional<ScenarioCall> call;
  std::optional<ScenarioDuress> duress;
  std::vector<ScenarioFact> facts;
  std::vector<ScenarioFact> oracle;
  std::vector<std::string> leakTokens;
  std::vector<ScenarioTurn> turns;
};

struct LoadedScenarios
{
  std::vector<ScenarioCase> cases;
  std::string error;
};

LoadedScenarios loadScenarios(const std::string& path);

LoadedScenarios loadScenarioPack(const std::vector<std::string>& paths);

std::vector<std::string> scenarioNames(const std::vector<ScenarioCase>& cases);

}
