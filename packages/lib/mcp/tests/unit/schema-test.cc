#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "test-support.hxx"

#include <mcp/json-rpc.hxx>
#include <mcp/schema.hxx>

#include <optional>
#include <string>

using namespace argus::mcp;
using test_support::json;

namespace
{
std::optional<std::string> check(const Json::Value& schema, const std::string& value)
{
  return schema::violation(schema, json(value));
}

Json::Value recallSchema()
{
  return schema::object({{.name = "query", .schema = schema::text(), .required = true},
                         {.name = "limit", .schema = schema::integer({.description = "", .minimum = 1, .maximum = 10}), .required = false},
                         {.name = "kind", .schema = schema::choice({"persona", "schedule"}), .required = false}});
}
}

TEST_CASE("a conforming object has no violation")
{
  CHECK_FALSE(check(recallSchema(), R"({"query":"dentista","limit":3,"kind":"schedule"})").has_value());
  CHECK_FALSE(check(recallSchema(), R"({"query":"x","unknown":true})").has_value());
}

TEST_CASE("a value that is not an object is refused with the wording the tool runtime has always used")
{
  CHECK(schema::violation(recallSchema(), Json::Value("guardalo")) ==
        std::optional<std::string>("arguments must be a JSON object"));
  CHECK(check(recallSchema(), "[1]") == std::optional<std::string>("arguments must be a JSON object"));
}

TEST_CASE("a missing required property is named")
{
  CHECK(check(recallSchema(), "{}") == std::optional<std::string>("missing required argument 'query'"));
}

TEST_CASE("a wrong type is named with its article")
{
  CHECK(check(recallSchema(), R"({"query":42})") == std::optional<std::string>("argument 'query' must be a string"));
  CHECK(check(recallSchema(), R"({"query":"x","limit":"3"})") ==
        std::optional<std::string>("argument 'limit' must be an integer"));
  CHECK(check(recallSchema(), R"({"query":"x","limit":2.5})") ==
        std::optional<std::string>("argument 'limit' must be an integer"));
  CHECK_FALSE(check(recallSchema(), R"({"query":"x","limit":2.0})").has_value());
}

TEST_CASE("an enum rejects a value outside its set")
{
  CHECK(check(recallSchema(), R"({"query":"x","kind":"inventado"})") ==
        std::optional<std::string>("argument 'kind' has an invalid value"));
}

TEST_CASE("numeric bounds are inclusive")
{
  CHECK_FALSE(check(recallSchema(), R"({"query":"x","limit":1})").has_value());
  CHECK_FALSE(check(recallSchema(), R"({"query":"x","limit":10})").has_value());
  CHECK(check(recallSchema(), R"({"query":"x","limit":0})").has_value());
  CHECK(check(recallSchema(), R"({"query":"x","limit":11})").has_value());
}

TEST_CASE("string length counts characters, not bytes")
{
  const Json::Value limited = schema::object({{.name = "name", .schema = schema::text({.description = "", .minimum = 2, .maximum = 4}), .required = true}});
  CHECK_FALSE(check(limited, R"({"name":"ñañ"})").has_value());
  CHECK(check(limited, R"({"name":"ñ"})").has_value());
  CHECK(check(limited, R"({"name":"ñañañ"})").has_value());
}

TEST_CASE("arrays are checked by size and by item")
{
  const Json::Value listed = schema::object(
      {{.name = "ids", .schema = schema::list(schema::integer(), {.description = "", .minimum = 1, .maximum = 3}), .required = true}});
  CHECK_FALSE(check(listed, R"({"ids":[1,2]})").has_value());
  CHECK(check(listed, R"({"ids":[]})").has_value());
  CHECK(check(listed, R"({"ids":[1,2,3,4]})").has_value());
  CHECK(check(listed, R"({"ids":[1,"x"]})") == std::optional<std::string>("argument 'ids[1]' must be an integer"));
}

TEST_CASE("nested objects report the dotted path")
{
  const Json::Value nested = schema::object(
      {{.name = "when", .schema = schema::object({{.name = "at", .schema = schema::text(), .required = true}}), .required = true}});
  CHECK(check(nested, R"({"when":{}})") == std::optional<std::string>("missing required argument 'when.at'"));
  CHECK(check(nested, R"({"when":{"at":3}})") == std::optional<std::string>("argument 'when.at' must be a string"));
}

TEST_CASE("an empty object schema accepts nothing else")
{
  CHECK_FALSE(check(schema::emptyObject(), "{}").has_value());
  CHECK(check(schema::emptyObject(), R"({"x":1})") == std::optional<std::string>("argument 'x' is not accepted"));
}

TEST_CASE("composition keywords are honoured")
{
  const Json::Value any = json(R"({"type":"object","properties":{"v":{"anyOf":[{"type":"string"},{"type":"integer"}]}}})");
  CHECK_FALSE(check(any, R"({"v":"a"})").has_value());
  CHECK_FALSE(check(any, R"({"v":3})").has_value());
  CHECK(check(any, R"({"v":true})").has_value());
  const Json::Value one = json(R"({"type":"object","properties":{"v":{"oneOf":[{"type":"integer"},{"type":"number"}]}}})");
  CHECK(check(one, R"({"v":3})").has_value());
  CHECK_FALSE(check(one, R"({"v":3.5})").has_value());
}

TEST_CASE("a schema nested past the depth bound is refused rather than followed")
{
  Json::Value deep = json(R"({"type":"string"})");
  for (int level = 0; level < 40; ++level) {
    Json::Value wrap(Json::objectValue);
    wrap["type"] = "object";
    wrap["properties"]["a"] = deep;
    deep = wrap;
  }
  Json::Value value(Json::objectValue);
  Json::Value* cursor = &value;
  for (int level = 0; level < 40; ++level) {
    (*cursor)["a"] = Json::Value(Json::objectValue);
    cursor = &(*cursor)["a"];
  }
  CHECK(schema::unsupported(deep).has_value());
  CHECK(schema::violation(deep, value).has_value());
}

TEST_CASE("an input schema must be an object schema without references or foreign dialects")
{
  CHECK_FALSE(schema::unsupported(recallSchema()).has_value());
  CHECK_FALSE(schema::unsupported(schema::emptyObject()).has_value());
  CHECK(schema::unsupported(json(R"({"type":"string"})")).has_value());
  CHECK(schema::unsupported(Json::Value()).has_value());
  CHECK(schema::unsupported(json(R"({"type":"object","properties":{"a":{"$ref":"https://example.com/x"}}})")).has_value());
  CHECK(schema::unsupported(json(R"({"type":"object","$schema":"http://json-schema.org/draft-04/schema#"})")).has_value());
  CHECK_FALSE(schema::unsupported(json(R"({"type":"object","$schema":"https://json-schema.org/draft/2020-12/schema"})")).has_value());
}

TEST_CASE("the builders describe themselves")
{
  const Json::Value property = schema::text({.description = "the dentist", .minimum = std::nullopt, .maximum = std::nullopt});
  CHECK(property["type"].asString() == "string");
  CHECK(property["description"].asString() == "the dentist");
  const Json::Value allowed = schema::choice({"a", "b"});
  CHECK(allowed["enum"].size() == 2);
  const Json::Value built = schema::object({{.name = "a", .schema = schema::boolean(), .required = true},
                                            {.name = "b", .schema = schema::number(), .required = false}});
  CHECK(built["required"].size() == 1);
  CHECK(built["required"][0].asString() == "a");
  CHECK(built["properties"]["b"]["type"].asString() == "number");
}
