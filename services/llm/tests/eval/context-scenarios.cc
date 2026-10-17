#include "context-scenarios.hxx"

#include <json/reader.h>
#include <json/value.h>

#include <algorithm>
#include <fstream>
#include <sstream>

namespace context_eval
{

namespace
{

std::vector<std::string> stringList(const Json::Value& node)
{
  std::vector<std::string> out;
  for (const auto& item : node)
    out.push_back(item.asString());
  return out;
}

std::vector<std::vector<std::string>> groupList(const Json::Value& node)
{
  std::vector<std::vector<std::string>> out;
  for (const auto& group : node) {
    if (group.isArray())
      out.push_back(stringList(group));
    else if (group.isString())
      out.push_back({group.asString()});
  }
  return out;
}

std::vector<ScenarioFact> factList(const Json::Value& node)
{
  std::vector<ScenarioFact> out;
  for (const auto& fact : node) {
    if (!fact.isObject())
      continue;
    ScenarioFact parsed{.facet = fact.get("facet", "").asString(), .text = fact.get("text", "").asString()};
    if (!parsed.text.empty())
      out.push_back(std::move(parsed));
  }
  return out;
}

ScenarioTurn parseTurn(const Json::Value& node)
{
  ScenarioTurn turn;
  turn.user = node.get("user", "").asString();
  turn.keep = groupList(node["keep"]);
  turn.first = groupList(node["first"]);
  turn.never = stringList(node["never"]);
  turn.noRepeat = stringList(node["noRepeat"]);
  turn.act = node.get("act", "").asString();
  turn.refuse = node.get("refuse", false).asBool();
  turn.moduleOff = node.get("moduleOff", false).asBool();
  turn.conflict = node.get("conflict", false).asBool();
  turn.partial = node.get("partial", false).asBool();
  turn.question = node.get("question", false).asBool();
  turn.noQuestion = node.get("noQuestion", false).asBool();
  turn.bargeIn = node.get("bargeIn", false).asBool();
  turn.duressHold = node.get("duressHold", false).asBool();
  turn.maxSentences = node.get("maxSentences", 3).asInt();
  turn.maxWords = node.get("maxWords", 45).asInt();
  return turn;
}

std::optional<ScenarioCall> parseCall(const Json::Value& node)
{
  if (!node.isObject())
    return std::nullopt;
  ScenarioCall call;
  call.trigger = node.get("trigger", "").asString();
  call.opening = node.get("opening", "").asString();
  call.reason = groupList(node["reason"]);
  return call;
}

std::optional<ScenarioDuress> parseDuress(const Json::Value& node)
{
  if (!node.isObject())
    return std::nullopt;
  ScenarioDuress duress;
  duress.phrase = node.get("phrase", "").asString();
  duress.turn = node.get("turn", 0).asInt();
  if (duress.phrase.empty())
    return std::nullopt;
  return duress;
}

ScenarioCase parseCase(const Json::Value& node)
{
  ScenarioCase out;
  out.id = node.get("id", "").asString();
  out.scenario = node.get("scenario", "").asString();
  out.axis = node.get("axis", "scenario").asString();
  out.locale = node.get("locale", "es").asString();
  out.lang = node.get("lang", "es").asString();
  out.variant = node.get("variant", "neutral").asString();
  out.role = node.get("role", "owner").asString();
  out.route = node.get("route", "lan").asString();
  out.name = node.get("name", "").asString();
  out.person = node.get("person", "").asString();
  out.modulesOff = stringList(node["modulesOff"]);
  out.call = parseCall(node["call"]);
  out.duress = parseDuress(node["duress"]);
  out.facts = factList(node["facts"]);
  out.oracle = factList(node["oracle"]);
  out.leakTokens = stringList(node["leak"]);
  for (const auto& turn : node["turns"])
    if (turn.isObject())
      out.turns.push_back(parseTurn(turn));
  return out;
}

bool appendFile(const std::string& path, LoadedScenarios& loaded)
{
  std::ifstream in(path);
  if (!in) {
    loaded.error = "cannot open " + path;
    return false;
  }
  Json::CharReaderBuilder builder;
  std::string line;
  size_t lineNumber = 0;
  while (std::getline(in, line)) {
    ++lineNumber;
    if (line.empty())
      continue;
    Json::Value node;
    std::string errors;
    std::istringstream stream(line);
    if (!Json::parseFromStream(builder, stream, &node, &errors)) {
      loaded.error = path + ":" + std::to_string(lineNumber) + ": " + errors;
      return false;
    }
    loaded.cases.push_back(parseCase(node));
  }
  return true;
}

}

LoadedScenarios loadScenarios(const std::string& path)
{
  LoadedScenarios loaded;
  if (!appendFile(path, loaded))
    loaded.cases.clear();
  return loaded;
}

LoadedScenarios loadScenarioPack(const std::vector<std::string>& paths)
{
  LoadedScenarios loaded;
  for (const std::string& path : paths) {
    if (!appendFile(path, loaded)) {
      loaded.cases.clear();
      return loaded;
    }
  }
  return loaded;
}

std::vector<std::string> scenarioNames(const std::vector<ScenarioCase>& cases)
{
  std::vector<std::string> out;
  for (const ScenarioCase& item : cases)
    if (!item.scenario.empty() && std::ranges::find(out, item.scenario) == out.end())
      out.push_back(item.scenario);
  return out;
}

}
