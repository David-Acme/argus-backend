#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <text/json-diff.hxx>
#include <text/json-util.hxx>

namespace {

Json::Value row(const char* text)
{
  return json_util::fromString(text);
}

}

TEST_CASE("a field cleared to null keeps its current key on the wire")
{
  const auto diff = JsonDiff::createFlatDiff(row(R"({"assigneeId":7})"),
                                             row(R"({"assigneeId":null})"));
  const auto wire = JsonDiff::toJson(diff);

  REQUIRE(wire.isMember("assigneeId"));
  CHECK(wire["assigneeId"]["previous"].asInt() == 7);
  CHECK(wire["assigneeId"].isMember("current"));
  CHECK(wire["assigneeId"]["current"].isNull());
}

TEST_CASE("a field set from null keeps its previous key on the wire")
{
  const auto diff = JsonDiff::createFlatDiff(row(R"({"endsAt":null})"),
                                             row(R"({"endsAt":42})"));
  const auto wire = JsonDiff::toJson(diff);

  CHECK(wire["endsAt"].isMember("previous"));
  CHECK(wire["endsAt"]["previous"].isNull());
  CHECK(wire["endsAt"]["current"].asInt() == 42);
}

TEST_CASE("the wire form round-trips through fromJsonString")
{
  const auto diff = JsonDiff::createFlatDiff(
      row(R"({"name":"a","due":5})"), row(R"({"name":"b","due":null})"));
  const auto parsed =
      JsonDiff::fromJsonString(json_util::toString(JsonDiff::toJson(diff)));

  REQUIRE(parsed.size() == 2);
  CHECK(parsed.at("name").current.asString() == "b");
  CHECK(parsed.at("due").previous.asInt() == 5);
  CHECK(parsed.at("due").current.isNull());
}

TEST_CASE("a payload nested past the reader's stack limit is not JSON, and parsing it never throws")
{
  const std::string deep = std::string(5000, '[') + std::string(5000, ']');
  CHECK_NOTHROW(static_cast<void>(json_util::fromString(deep)));
  CHECK(json_util::fromString(deep).isNull());
  CHECK_FALSE(json_util::isValid(deep));
  CHECK(json_util::fromString("not-json").isNull());
  CHECK(json_util::fromString(R"({"a":1})")["a"].asInt() == 1);
}
