#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <mcp/speech.hxx>

#include <string>
#include <vector>

using namespace argus::mcp;

namespace
{
ToolInvocation by(const std::string& lang)
{
  return {.name = "x", .arguments = Json::Value(Json::objectValue), .caller = {.userId = 1, .role = "owner", .lang = lang, .sessionId = "", .utterance = "", .decided = false}};
}
}

TEST_CASE("the words come back in the user's language and Spanish is the default")
{
  const ToolInvocation english = by("en");
  const ToolInvocation spanish = by("es");
  const ToolInvocation unset = by("");
  CHECK(speech::say({.invocation = english, .spanish = "hola", .english = "hello"}) == "hello");
  CHECK(speech::say({.invocation = spanish, .spanish = "hola", .english = "hello"}) == "hola");
  CHECK(speech::say({.invocation = unset, .spanish = "hola", .english = "hello"}) == "hola");
  CHECK(speech::languageOf(english) == "en");
  CHECK(speech::languageOf(unset) == "es");
  CHECK(speech::inEnglish(english));
  CHECK_FALSE(speech::inEnglish(spanish));
}

TEST_CASE("a refusal is an error outcome with the words and the code")
{
  const ToolInvocation english = by("en");
  const auto outcome = speech::refuse({.invocation = english, .spanish = "no", .english = "no, sorry"}, "unknown_thing");
  CHECK(outcome.isError);
  CHECK(outcome.text == "no, sorry");
  CHECK(outcome.structured["code"].asString() == "unknown_thing");
}

TEST_CASE("a list is joined with commas and the language's last conjunction")
{
  CHECK(speech::joined({}, "es").empty());
  CHECK(speech::joined({"uno"}, "es") == "uno");
  CHECK(speech::joined({"uno", "dos"}, "es") == "uno y dos");
  CHECK(speech::joined({"uno", "dos", "tres"}, "es") == "uno, dos y tres");
  CHECK(speech::joined({"one", "two", "three"}, "en") == "one, two and three");
}
