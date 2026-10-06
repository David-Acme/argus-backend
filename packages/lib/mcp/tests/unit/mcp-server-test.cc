#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "test-support.hxx"

#include <mcp/json-rpc.hxx>
#include <mcp/schema.hxx>
#include <mcp/server.hxx>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

using namespace argus::mcp;
using test_support::answer;
using test_support::errorOf;
using test_support::must;
using test_support::resultOf;

namespace
{
std::string call(const McpServer& server, const std::string& method, Json::Value params, bool withMeta = true)
{
  if (withMeta) {
    Json::Value info(Json::objectValue);
    info["name"] = "test";
    const Json::Value meta = requestMeta(info);
    for (const auto& key : meta.getMemberNames())
      params["_meta"][key] = meta[key];
  }
  return server.handleBlocking(requestFrame({.hasId = true, .id = Json::Value(1), .method = method, .params = std::move(params)}));
}

Json::Value callParams(const std::string& name, Json::Value arguments)
{
  Json::Value params(Json::objectValue);
  params["name"] = name;
  params["arguments"] = std::move(arguments);
  return params;
}

McpServer makeServer()
{
  McpServer server({.name = "argus-test", .version = "1", .instructions = "Tools for tests"});
  server.addSync({.name = "memory.recall",
                  .title = "Recall",
                  .description = "Recalls what was saved",
                  .inputSchema = schema::object({{.name = "query", .schema = schema::text(), .required = true}}),
                  .annotations = {.readOnly = true},
                  .module = "core",
                  .capability = "memory.read"},
                 [](const ToolInvocation& invocation) {
                   ToolOutcome outcome;
                   outcome.text = "recalled " + invocation.arguments["query"].asString() + " for " +
                                  std::to_string(invocation.caller.userId) + " " + invocation.caller.role + " " +
                                  invocation.caller.lang + " " + invocation.caller.utterance;
                   outcome.structured["user"] = invocation.caller.userId;
                   return outcome;
                 });
  server.addSync({.name = "app.open",
                  .title = "",
                  .description = "Opens a screen",
                  .inputSchema = schema::object({{.name = "screen", .schema = schema::choice({"home", "agenda"}), .required = true}}),
                  .annotations = {},
                  .module = "core",
                  .capability = "app.open"},
                 [](const ToolInvocation& invocation) {
                   ToolOutcome outcome;
                   outcome.text = "opened";
                   outcome.appAction = AppAction{.name = "app.open", .arguments = invocation.arguments};
                   return outcome;
                 });
  return server;
}
}

TEST_CASE("discover names the protocol version, the identity and the tools capability")
{
  const McpServer server = makeServer();
  const Json::Value result = resultOf(answer(call(server, "server/discover", Json::Value(Json::objectValue))));
  CHECK(result["resultType"].asString() == "complete");
  CHECK(result["supportedVersions"][0].asString() == "2026-07-28");
  CHECK(result["capabilities"]["tools"]["listChanged"].asBool() == false);
  CHECK(result["_meta"]["io.modelcontextprotocol/serverInfo"]["name"].asString() == "argus-test");
  CHECK(result["instructions"].asString() == "Tools for tests");
  CHECK(result["ttlMs"].asInt64() >= 0);
  CHECK(result["cacheScope"].asString() == "public");
}

TEST_CASE("tools/list is deterministic and carries the module and capability of every tool")
{
  const McpServer server = makeServer();
  const auto first = call(server, "tools/list", Json::Value(Json::objectValue));
  CHECK(first == call(server, "tools/list", Json::Value(Json::objectValue)));
  const Json::Value listed = resultOf(answer(first));
  const Json::Value& tools = listed["tools"];
  REQUIRE(tools.size() == 2);
  CHECK(tools[0]["name"].asString() == "app.open");
  CHECK(tools[1]["name"].asString() == "memory.recall");
  CHECK(tools[1]["_meta"]["argus/module"].asString() == "core");
  CHECK(tools[1]["_meta"]["argus/capability"].asString() == "memory.read");
  CHECK(tools[1]["annotations"]["readOnlyHint"].asBool());
  CHECK(listed["resultType"].asString() == "complete");
  CHECK_FALSE(listed.isMember("nextCursor"));
}

TEST_CASE("a call carries its arguments and the declared caller to the handler")
{
  const McpServer server = makeServer();
  Json::Value params = callParams("memory.recall", Json::Value(Json::objectValue));
  params["arguments"]["query"] = "dentista";
  params["_meta"]["argus/context"] =
      toJson(CallerContext{.userId = 7, .role = "resident", .lang = "es", .sessionId = "s", .utterance = "qué dije", .decided = false});
  const Json::Value result = resultOf(answer(call(server, "tools/call", params)));
  const auto outcome = must(toolOutcomeFrom(result));
  CHECK_FALSE(outcome.isError);
  CHECK(outcome.text == "recalled dentista for 7 resident es qué dije");
  CHECK(outcome.structured["user"].asInt64() == 7);
  CHECK(result["_meta"]["io.modelcontextprotocol/serverInfo"]["name"].asString() == "argus-test");
}

TEST_CASE("an app action travels in the result _meta")
{
  const McpServer server = makeServer();
  Json::Value arguments(Json::objectValue);
  arguments["screen"] = "agenda";
  const auto outcome =
      must(toolOutcomeFrom(resultOf(answer(call(server, "tools/call", callParams("app.open", arguments))))));
  const auto action = must(outcome.appAction);
  CHECK(action.name == "app.open");
  CHECK(action.arguments["screen"].asString() == "agenda");
}

TEST_CASE("arguments the schema refuses come back as a tool error the model can read, and never reach the handler")
{
  std::atomic<int> ran{0};
  McpServer server({.name = "t", .version = "1", .instructions = ""});
  server.addSync({.name = "memory.forget",
                  .title = "",
                  .description = "",
                  .inputSchema = schema::object({{.name = "query", .schema = schema::text(), .required = true}}),
                  .annotations = {},
                  .module = "core",
                  .capability = ""},
                 [&ran](const ToolInvocation&) {
                   ++ran;
                   return ToolOutcome{};
                 });
  const auto outcome = must(toolOutcomeFrom(
      resultOf(answer(call(server, "tools/call", callParams("memory.forget", Json::Value(Json::objectValue)))))));
  CHECK(outcome.isError);
  CHECK(outcome.text == "missing required argument 'query'");
  CHECK(outcome.structured["code"].asString() == "invalid_arguments");
  CHECK(ran == 0);
}

TEST_CASE("an unknown tool is a protocol error")
{
  const McpServer server = makeServer();
  const auto error = errorOf(answer(call(server, "tools/call", callParams("nope.tool", Json::Value(Json::objectValue)))));
  CHECK(error.code == codeOf(ErrorCode::InvalidParams));
  CHECK(error.message == "Unknown tool: nope.tool");
}

TEST_CASE("malformed call params are protocol errors")
{
  const McpServer server = makeServer();
  Json::Value nameless(Json::objectValue);
  CHECK(errorOf(answer(call(server, "tools/call", nameless))).code == codeOf(ErrorCode::InvalidParams));
  Json::Value params = callParams("memory.recall", Json::Value("text"));
  CHECK(errorOf(answer(call(server, "tools/call", params))).code == codeOf(ErrorCode::InvalidParams));
}

TEST_CASE("a request without the per-request meta is invalid params")
{
  const McpServer server = makeServer();
  const auto error = errorOf(answer(call(server, "tools/list", Json::Value(Json::objectValue), false)));
  CHECK(error.code == codeOf(ErrorCode::InvalidParams));
}

TEST_CASE("a protocol version the server does not speak names the ones it does")
{
  const McpServer server = makeServer();
  Json::Value params(Json::objectValue);
  params["_meta"]["io.modelcontextprotocol/protocolVersion"] = "1900-01-01";
  params["_meta"]["io.modelcontextprotocol/clientCapabilities"] = Json::Value(Json::objectValue);
  const auto error = errorOf(answer(call(server, "tools/list", params, false)));
  CHECK(error.code == codeOf(ErrorCode::UnsupportedProtocolVersion));
  CHECK(error.data["supported"][0].asString() == "2026-07-28");
  CHECK(error.data["requested"].asString() == "1900-01-01");
}

TEST_CASE("initialize is refused and names the version this server speaks")
{
  const McpServer server = makeServer();
  const auto error = errorOf(answer(call(server, "initialize", Json::Value(Json::objectValue), false)));
  CHECK(error.code == codeOf(ErrorCode::MethodNotFound));
  CHECK(error.data["supported"][0].asString() == "2026-07-28");
}

TEST_CASE("an unknown method is method not found")
{
  const McpServer server = makeServer();
  CHECK(errorOf(answer(call(server, "resources/list", Json::Value(Json::objectValue)))).code ==
        codeOf(ErrorCode::MethodNotFound));
}

TEST_CASE("a notification gets no answer")
{
  const McpServer server = makeServer();
  CHECK(server.handleBlocking(R"({"jsonrpc":"2.0","method":"notifications/cancelled","params":{}})").empty());
}

TEST_CASE("a frame that is not JSON gets a parse error")
{
  const McpServer server = makeServer();
  CHECK(errorOf(answer(server.handleBlocking("{nope"))).code == codeOf(ErrorCode::ParseError));
}

TEST_CASE("the gate refuses a call with its own code and the handler never runs")
{
  std::atomic<int> ran{0};
  McpServer server({.name = "t", .version = "1", .instructions = ""});
  server.addSync({.name = "calendar.create_event",
                  .title = "",
                  .description = "",
                  .inputSchema = schema::emptyObject(),
                  .annotations = {},
                  .module = "productivity",
                  .capability = "agenda.write"},
                 [&ran](const ToolInvocation&) {
                   ++ran;
                   return ToolOutcome{};
                 });
  std::vector<std::string> seen;
  server.setGate([&seen](const ToolInvocation& invocation, const ToolSpec& spec) -> std::optional<Refusal> {
    seen.push_back(spec.module + ":" + spec.capability + ":" + invocation.caller.role);
    if (invocation.caller.role == "guest")
      return Refusal{.code = "forbidden", .message = "permission denied for tool: " + spec.name};
    return std::nullopt;
  });
  Json::Value params = callParams("calendar.create_event", Json::Value(Json::objectValue));
  params["_meta"]["argus/context"] = toJson(CallerContext{.userId = 2, .role = "guest", .lang = "es", .sessionId = "", .utterance = "", .decided = false});
  const auto outcome = must(toolOutcomeFrom(resultOf(answer(call(server, "tools/call", params)))));
  CHECK(outcome.isError);
  CHECK(outcome.text == "permission denied for tool: calendar.create_event");
  CHECK(outcome.structured["code"].asString() == "forbidden");
  CHECK(ran == 0);
  params["_meta"]["argus/context"]["role"] = "owner";
  CHECK_FALSE(must(toolOutcomeFrom(resultOf(answer(call(server, "tools/call", params))))).isError);
  CHECK(ran == 1);
  CHECK(seen == std::vector<std::string>{"productivity:agenda.write:guest", "productivity:agenda.write:owner"});
}

TEST_CASE("a handler that throws becomes a tool error and the server keeps serving")
{
  McpServer server({.name = "t", .version = "1", .instructions = ""});
  server.addSync({.name = "boom", .title = "", .description = "", .inputSchema = schema::emptyObject(), .annotations = {}, .module = "", .capability = ""},
                 [](const ToolInvocation&) -> ToolOutcome { throw std::runtime_error("secret detail"); });
  const auto outcome = must(toolOutcomeFrom(
      resultOf(answer(call(server, "tools/call", callParams("boom", Json::Value(Json::objectValue)))))));
  CHECK(outcome.isError);
  CHECK(outcome.text.find("secret detail") == std::string::npos);
  CHECK(answer(call(server, "tools/list", Json::Value(Json::objectValue))).result.has_value());
}

TEST_CASE("an asynchronous handler answers from another thread, once")
{
  McpServer server({.name = "t", .version = "1", .instructions = ""});
  std::vector<std::jthread> workers;
  server.add({.name = "slow", .title = "", .description = "", .inputSchema = schema::emptyObject(), .annotations = {}, .module = "", .capability = ""},
             [&workers](const ToolInvocation&, McpServer::Reply reply) {
               workers.emplace_back([reply = std::move(reply)] {
                 std::this_thread::sleep_for(std::chrono::milliseconds(20));
                 ToolOutcome first;
                 first.text = "first";
                 reply(first);
                 ToolOutcome second;
                 second.text = "second";
                 reply(second);
               });
             });
  CHECK(must(toolOutcomeFrom(resultOf(answer(call(server, "tools/call", callParams("slow", Json::Value(Json::objectValue))))))).text ==
        "first");
}

TEST_CASE("a handler that never answers times out with an internal error")
{
  McpServer server({.name = "t", .version = "1", .instructions = ""});
  server.add({.name = "mute", .title = "", .description = "", .inputSchema = schema::emptyObject(), .annotations = {}, .module = "", .capability = ""},
             [](const ToolInvocation&, const McpServer::Reply&) {});
  Json::Value info(Json::objectValue);
  Json::Value params = callParams("mute", Json::Value(Json::objectValue));
  const Json::Value meta = requestMeta(info);
  for (const auto& key : meta.getMemberNames())
    params["_meta"][key] = meta[key];
  const auto error = errorOf(answer(server.handleBlocking(
      requestFrame({.hasId = true, .id = Json::Value(1), .method = "tools/call", .params = params}), std::chrono::milliseconds(50))));
  CHECK(error.code == codeOf(ErrorCode::InternalError));
}

TEST_CASE("registration refuses what could not be served")
{
  McpServer server({.name = "t", .version = "1", .instructions = ""});
  const ToolSpec good{.name = "ok", .title = "", .description = "", .inputSchema = schema::emptyObject(), .annotations = {}, .module = "", .capability = ""};
  const auto handler = [](const ToolInvocation&) { return ToolOutcome{}; };
  server.addSync(good, handler);
  CHECK_THROWS_AS(server.addSync(good, handler), std::invalid_argument);
  ToolSpec badName = good;
  badName.name = "has space";
  CHECK_THROWS_AS(server.addSync(badName, handler), std::invalid_argument);
  ToolSpec badSchema = good;
  badSchema.name = "refs";
  badSchema.inputSchema = Json::Value(Json::objectValue);
  badSchema.inputSchema["type"] = "object";
  badSchema.inputSchema["properties"]["a"]["$ref"] = "https://example.com/a";
  CHECK_THROWS_AS(server.addSync(badSchema, handler), std::invalid_argument);
  ToolSpec noHandler = good;
  noHandler.name = "empty";
  CHECK_THROWS_AS(server.addSync(noHandler, McpServer::SyncHandler{}), std::invalid_argument);
  CHECK(server.tools().size() == 1);
  CHECK(server.find("ok") != nullptr);
  CHECK(server.find("refs") == nullptr);
}
