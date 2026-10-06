#include "eval-case.hxx"

#include <json/reader.h>

#include <fstream>
#include <sstream>

namespace eval
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

ArgMatcher parseMatcher(const Json::Value& node)
{
  ArgMatcher matcher;
  matcher.name = node.get("arg", "").asString();
  matcher.equals = node.get("equals", "").asString();
  matcher.anyNeedles = stringList(node["anyValueContains"]);
  matcher.allNeedles = stringList(node["allNeedles"]);
  matcher.pattern = node.get("anyValueRegex", "").asString();
  return matcher;
}

std::vector<ArgMatcher> parseMatchers(const Json::Value& node)
{
  std::vector<ArgMatcher> out;
  for (const auto& item : node)
    out.push_back(parseMatcher(item));
  return out;
}

EvalCase parseCase(const Json::Value& node)
{
  EvalCase out;
  out.id = node["id"].asString();
  out.group = node.get("group", "").asString();
  out.lang = node["lang"].asString();
  out.variant = node["variant"].asString();
  out.role = node["role"].asString();
  out.modules = stringList(node["modules"]);
  out.script = stringList(node["script"]);
  out.route = node["route"].asString();
  out.twin = node.get("twin", "").asString();
  const Json::Value& expect = node["expect"];
  for (const auto& call : expect["calls"])
    out.calls.push_back({.tool = call["tool"].asString(), .args = parseMatchers(call["args"])});
  out.allowed = stringList(expect["allowed"]);
  if (expect.isMember("inactive")) {
    const Json::Value& inactive = expect["inactive"];
    out.inactive = InactiveExpectation{.module = inactive["module"].asString(),
                                       .attempted = inactive["attempted"].asString(),
                                       .audience = inactive["audience"].asString()};
  }
  if (expect.isMember("confirm")) {
    const Json::Value& confirm = expect["confirm"];
    out.confirm = ConfirmExpectation{.tool = confirm["tool"].asString(),
                                     .phase = confirm["phase"].asString(),
                                     .args = parseMatchers(confirm["args"])};
  }
  if (expect.isMember("offerAccept")) {
    const Json::Value& accept = expect["offerAccept"];
    out.offerAccept = OfferAccept{.module = accept["module"].asString(),
                                  .tool = accept["tool"].asString(),
                                  .attempted = accept["attempted"].asString()};
  }
  if (expect.isMember("offerDecline")) {
    out.offerDecline = true;
    out.offerDeclineModule = expect["offerDecline"]["module"].asString();
  }
  return out;
}

}

LoadedCases loadCases(const std::string& path)
{
  LoadedCases loaded;
  std::ifstream in(path);
  if (!in) {
    loaded.error = "cannot open " + path;
    return loaded;
  }
  std::string line;
  size_t lineNumber = 0;
  Json::CharReaderBuilder builder;
  while (std::getline(in, line)) {
    ++lineNumber;
    if (line.empty())
      continue;
    Json::Value node;
    std::string errors;
    std::istringstream stream(line);
    if (!Json::parseFromStream(builder, stream, &node, &errors)) {
      loaded.error = path;
      loaded.error += ':';
      loaded.error += std::to_string(lineNumber);
      loaded.error += ": ";
      loaded.error += errors;
      loaded.cases.clear();
      return loaded;
    }
    loaded.cases.push_back(parseCase(node));
  }
  return loaded;
}

}
