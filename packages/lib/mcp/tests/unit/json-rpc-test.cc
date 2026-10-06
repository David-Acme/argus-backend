#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "test-support.hxx"

#include <mcp/json-rpc.hxx>

#include <string>
#include <variant>

using namespace argus::mcp;
using namespace test_support;

namespace
{
RpcRequest asRequest(const ParsedRequest& parsed)
{
  REQUIRE(std::holds_alternative<RpcRequest>(parsed));
  return std::get<RpcRequest>(parsed);
}

RpcRejection asRejection(const ParsedRequest& parsed)
{
  REQUIRE(std::holds_alternative<RpcRejection>(parsed));
  return std::get<RpcRejection>(parsed);
}

int rejectedWith(const std::string& frame)
{
  return asRejection(parseRequest(frame)).error.code;
}
}

TEST_CASE("a request with an id, a method and params is parsed whole")
{
  const auto request = asRequest(parseRequest(R"({"jsonrpc":"2.0","id":7,"method":"tools/list","params":{"cursor":"a"}})"));
  CHECK(request.hasId);
  CHECK(request.id.asInt64() == 7);
  CHECK(request.method == "tools/list");
  CHECK(request.params["cursor"].asString() == "a");
  CHECK_FALSE(request.notification());
}

TEST_CASE("a string id is kept as it came")
{
  CHECK(asRequest(parseRequest(R"({"jsonrpc":"2.0","id":"abc","method":"x"})")).id.asString() == "abc");
}

TEST_CASE("a frame without an id is a notification and absent params are an empty object")
{
  const auto request = asRequest(parseRequest(R"({"jsonrpc":"2.0","method":"notifications/x"})"));
  CHECK(request.notification());
  CHECK(request.params.isObject());
  CHECK(request.params.empty());
}

TEST_CASE("a frame that is not JSON is a parse error with no id")
{
  const auto rejection = asRejection(parseRequest("{not json"));
  CHECK(rejection.error.code == codeOf(ErrorCode::ParseError));
  CHECK(rejection.id.isNull());
}

TEST_CASE("a frame nested past the stack limit is a parse error and never throws")
{
  const std::string deep = std::string(5000, '[') + std::string(5000, ']');
  CHECK(rejectedWith(deep) == codeOf(ErrorCode::ParseError));
}

TEST_CASE("the shapes JSON-RPC forbids are invalid requests")
{
  const int invalid = codeOf(ErrorCode::InvalidRequest);
  CHECK(rejectedWith("[1,2]") == invalid);
  CHECK(rejectedWith(R"({"id":1,"method":"x"})") == invalid);
  CHECK(rejectedWith(R"({"jsonrpc":"1.0","id":1,"method":"x"})") == invalid);
  CHECK(rejectedWith(R"({"jsonrpc":"2.0","id":1})") == invalid);
  CHECK(rejectedWith(R"({"jsonrpc":"2.0","id":1,"method":""})") == invalid);
  CHECK(rejectedWith(R"({"jsonrpc":"2.0","id":null,"method":"x"})") == invalid);
  CHECK(rejectedWith(R"({"jsonrpc":"2.0","id":1.5,"method":"x"})") == invalid);
  CHECK(rejectedWith(R"({"jsonrpc":"2.0","id":[1],"method":"x"})") == invalid);
}

TEST_CASE("params that are not an object are invalid params and keep the request id")
{
  const auto rejection = asRejection(parseRequest(R"({"jsonrpc":"2.0","id":3,"method":"x","params":[1]})"));
  CHECK(rejection.error.code == codeOf(ErrorCode::InvalidParams));
  CHECK(rejection.id.asInt64() == 3);
}

TEST_CASE("a rejected frame still names the id it could read")
{
  CHECK(asRejection(parseRequest(R"({"jsonrpc":"2.0","id":"r-1","method":5})")).id.asString() == "r-1");
}

TEST_CASE("a result frame round trips through the response parser")
{
  Json::Value result(Json::objectValue);
  result["answer"] = "sí";
  const auto response = answer(resultFrame({.id = Json::Value(4), .result = result}));
  CHECK(response.id.asInt64() == 4);
  CHECK(resultOf(response)["answer"].asString() == "sí");
  CHECK_FALSE(response.error.has_value());
}

TEST_CASE("an error frame carries its code, message and data")
{
  Json::Value data(Json::objectValue);
  data["supported"] = "2026-07-28";
  const auto response = answer(errorFrame(
      Json::Value("e"), {.code = codeOf(ErrorCode::UnsupportedProtocolVersion), .message = "no", .data = data}));
  const auto error = errorOf(response);
  CHECK(error.code == -32022);
  CHECK(error.message == "no");
  CHECK(error.data["supported"].asString() == "2026-07-28");
  CHECK_FALSE(response.result.has_value());
}

TEST_CASE("a response that is neither a result nor an error is refused")
{
  CHECK_FALSE(parseResponse(R"({"jsonrpc":"2.0","id":1})").has_value());
  CHECK_FALSE(parseResponse(R"({"jsonrpc":"2.0","id":1,"result":3})").has_value());
  CHECK_FALSE(parseResponse(R"({"jsonrpc":"2.0","id":1,"error":{"code":"x","message":"m"}})").has_value());
  CHECK_FALSE(parseResponse("nope").has_value());
}

TEST_CASE("a request frame survives its own parser")
{
  RpcRequest request;
  request.hasId = true;
  request.id = Json::Value(11);
  request.method = "tools/call";
  request.params["name"] = "memory.recall";
  const auto back = asRequest(parseRequest(requestFrame(request)));
  CHECK(back.id.asInt64() == 11);
  CHECK(back.method == "tools/call");
  CHECK(back.params["name"].asString() == "memory.recall");
}

TEST_CASE("serialization is compact and keeps UTF-8 as it is")
{
  Json::Value value(Json::objectValue);
  value["text"] = "ñandú";
  CHECK(serialize(value) == "{\"text\":\"ñandú\"}");
}
