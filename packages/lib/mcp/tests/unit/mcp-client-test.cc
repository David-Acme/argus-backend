#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "test-support.hxx"

#include <mcp/client.hxx>
#include <mcp/local-transport.hxx>
#include <mcp/schema.hxx>
#include <mcp/server.hxx>

#include <deque>
#include <memory>
#include <optional>
#include <string>
#include <vector>

using namespace argus::mcp;
using test_support::must;
using test_support::valueOf;

namespace
{
class ScriptedTransport final : public Transport
{
public:
  std::deque<std::optional<std::string>> answers;
  std::vector<Json::Value> sent;

  [[nodiscard]] std::optional<std::string> exchange(const std::string& frame) override
  {
    sent.push_back(test_support::must(parseJson(frame)));
    if (answers.empty())
      return std::nullopt;
    auto next = answers.front();
    answers.pop_front();
    return next;
  }
};

std::shared_ptr<const McpServer> sampleServer()
{
  auto server = std::make_shared<McpServer>(ServerIdentity{.name = "argus-test", .version = "9", .instructions = "hi"});
  server->addSync({.name = "memory.recall",
                   .title = "Recall",
                   .description = "Recalls what was saved",
                   .inputSchema = schema::object({{.name = "query", .schema = schema::text(), .required = true}}),
                   .annotations = {.readOnly = true},
                   .module = "core",
                   .capability = "memory.read"},
                  [](const ToolInvocation& invocation) {
                    ToolOutcome outcome;
                    outcome.text = "recalled " + invocation.arguments["query"].asString() + " as " +
                                   std::to_string(invocation.caller.userId) + "/" + invocation.caller.role;
                    return outcome;
                  });
  return server;
}

McpClient clientOver(std::shared_ptr<Transport> transport)
{
  return McpClient(std::move(transport), {.name = "argus-llm", .version = "1"});
}
}

TEST_CASE("a client reads a server end to end over the in-process transport")
{
  const McpClient client = clientOver(std::make_shared<LocalTransport>(sampleServer()));

  const auto described = valueOf(client.discover());
  CHECK(described.name == "argus-test");
  CHECK(described.version == "9");
  CHECK(described.instructions == "hi");
  CHECK(described.tools);
  CHECK(described.supportedVersions == std::vector<std::string>{"2026-07-28"});

  const auto listed = valueOf(client.listTools());
  REQUIRE(listed.tools.size() == 1);
  CHECK(listed.tools[0].name == "memory.recall");
  CHECK(listed.tools[0].module == "core");
  CHECK(listed.tools[0].capability == "memory.read");
  CHECK(listed.tools[0].annotations.readOnly);
  CHECK(listed.ttlMs >= 0);

  Json::Value arguments(Json::objectValue);
  arguments["query"] = "dentista";
  const auto called = valueOf(client.callTool(
      {.name = "memory.recall",
       .arguments = arguments,
       .caller = {.userId = 5, .role = "owner", .lang = "es", .sessionId = "", .utterance = "", .decided = false}}));
  CHECK(called.text == "recalled dentista as 5/owner");
  CHECK_FALSE(called.isError);
}

TEST_CASE("a tool the server reports as failed is still an answer, not a failure of the call")
{
  const McpClient client = clientOver(std::make_shared<LocalTransport>(sampleServer()));
  const auto called =
      valueOf(client.callTool({.name = "memory.recall", .arguments = Json::Value(Json::objectValue), .caller = {}}));
  CHECK(called.isError);
  CHECK(called.text == "missing required argument 'query'");
}

TEST_CASE("an unknown tool is a protocol failure with the server's own words")
{
  const McpClient client = clientOver(std::make_shared<LocalTransport>(sampleServer()));
  const auto called = client.callTool({.name = "nope", .arguments = Json::Value(Json::objectValue), .caller = {}});
  CHECK_FALSE(called.ok());
  CHECK(called.failure.kind == Failure::Kind::Protocol);
  CHECK(called.failure.error.code == codeOf(ErrorCode::InvalidParams));
  CHECK(called.failure.error.message == "Unknown tool: nope");
}

TEST_CASE("every request carries the version, the client identity and its own id")
{
  auto transport = std::make_shared<ScriptedTransport>();
  const McpClient client = clientOver(transport);
  CHECK_FALSE(client.discover().ok());
  CHECK_FALSE(client.listTools().ok());
  REQUIRE(transport->sent.size() == 2);
  for (const auto& frame : transport->sent) {
    CHECK(frame["params"]["_meta"]["io.modelcontextprotocol/protocolVersion"].asString() == "2026-07-28");
    CHECK(frame["params"]["_meta"]["io.modelcontextprotocol/clientInfo"]["name"].asString() == "argus-llm");
    CHECK(frame["params"]["_meta"]["io.modelcontextprotocol/clientCapabilities"].isObject());
  }
  CHECK(transport->sent[0]["id"].asInt64() != transport->sent[1]["id"].asInt64());
}

TEST_CASE("the declared caller travels in the call's _meta")
{
  auto transport = std::make_shared<ScriptedTransport>();
  const McpClient client = clientOver(transport);
  CHECK_FALSE(client.callTool({.name = "x",
                               .arguments = Json::Value(Json::objectValue),
                               .caller = {.userId = 9, .role = "guest", .lang = "en", .sessionId = "s", .utterance = "u", .decided = true}})
                  .ok());
  const Json::Value& context = transport->sent.at(0)["params"]["_meta"]["argus/context"];
  CHECK(context["userId"].asInt64() == 9);
  CHECK(context["role"].asString() == "guest");
  CHECK(context["utterance"].asString() == "u");
  CHECK(context["decided"].asBool());
}

TEST_CASE("a transport that gives nothing back is a transport failure")
{
  auto transport = std::make_shared<ScriptedTransport>();
  const McpClient client = clientOver(transport);
  const auto listed = client.listTools();
  CHECK_FALSE(listed.ok());
  CHECK(listed.failure.kind == Failure::Kind::Transport);
  const McpClient detached = clientOver(nullptr);
  CHECK(detached.listTools().failure.kind == Failure::Kind::Transport);
}

TEST_CASE("an answer that is not a response, or answers another request, is malformed")
{
  auto transport = std::make_shared<ScriptedTransport>();
  const McpClient client = clientOver(transport);
  transport->answers.emplace_back("garbage");
  CHECK(client.listTools().failure.kind == Failure::Kind::Malformed);
  transport->answers.emplace_back(resultFrame({.id = Json::Value(9999), .result = Json::Value(Json::objectValue)}));
  CHECK(client.listTools().failure.kind == Failure::Kind::Malformed);
}

TEST_CASE("a tool list that is not an array is malformed")
{
  auto transport = std::make_shared<ScriptedTransport>();
  const McpClient client = clientOver(transport);
  Json::Value result(Json::objectValue);
  result["tools"] = "x";
  transport->answers.emplace_back(resultFrame({.id = Json::Value(1), .result = result}));
  CHECK(client.listTools().failure.kind == Failure::Kind::Malformed);
}

TEST_CASE("a paginated list is followed to its last page and entries that are not tools are skipped")
{
  auto transport = std::make_shared<ScriptedTransport>();
  const McpClient client = clientOver(transport);
  const auto page = [](const std::string& name, const std::string& next) {
    Json::Value result(Json::objectValue);
    result["resultType"] = "complete";
    Json::Value tools(Json::arrayValue);
    tools.append(toJson(ToolSpec{.name = name,
                                 .title = "",
                                 .description = "",
                                 .inputSchema = schema::emptyObject(),
                                 .annotations = {},
                                 .module = "",
                                 .capability = ""}));
    tools.append(Json::Value("junk"));
    result["tools"] = std::move(tools);
    result["ttlMs"] = 1000;
    if (!next.empty())
      result["nextCursor"] = next;
    return result;
  };
  transport->answers.emplace_back(resultFrame({.id = Json::Value(1), .result = page("one", "c2")}));
  transport->answers.emplace_back(resultFrame({.id = Json::Value(2), .result = page("two", "")}));
  const auto listed = valueOf(client.listTools());
  REQUIRE(listed.tools.size() == 2);
  CHECK(listed.tools[0].name == "one");
  CHECK(listed.tools[1].name == "two");
  CHECK(listed.ttlMs == 1000);
  REQUIRE(transport->sent.size() == 2);
  CHECK_FALSE(transport->sent[0]["params"].isMember("cursor"));
  CHECK(transport->sent[1]["params"]["cursor"].asString() == "c2");
}

TEST_CASE("a list that never ends is cut at the page bound")
{
  auto transport = std::make_shared<ScriptedTransport>();
  const McpClient client = clientOver(transport);
  for (int index = 1; index <= McpClient::kMaxPages + 1; ++index) {
    Json::Value result(Json::objectValue);
    result["tools"] = Json::Value(Json::arrayValue);
    result["nextCursor"] = "again";
    transport->answers.emplace_back(resultFrame({.id = Json::Value(index), .result = std::move(result)}));
  }
  const auto listed = client.listTools();
  CHECK_FALSE(listed.ok());
  CHECK(listed.failure.kind == Failure::Kind::Malformed);
  CHECK(transport->sent.size() == static_cast<size_t>(McpClient::kMaxPages));
}

TEST_CASE("an error response keeps its code for the caller to act on")
{
  auto transport = std::make_shared<ScriptedTransport>();
  const McpClient client = clientOver(transport);
  Json::Value data(Json::objectValue);
  data["supported"] = "2025-11-25";
  transport->answers.emplace_back(errorFrame(
      Json::Value(1), {.code = codeOf(ErrorCode::UnsupportedProtocolVersion), .message = "Unsupported protocol version", .data = data}));
  const auto described = client.discover();
  CHECK_FALSE(described.ok());
  CHECK(described.failure.kind == Failure::Kind::Protocol);
  CHECK(described.failure.error.code == -32022);
  CHECK(described.failure.error.data["supported"].asString() == "2025-11-25");
}

TEST_CASE("a call result that is not a tool result is malformed")
{
  auto transport = std::make_shared<ScriptedTransport>();
  const McpClient client = clientOver(transport);
  transport->answers.emplace_back(resultFrame({.id = Json::Value(1), .result = Json::Value(Json::objectValue)}));
  CHECK(client.callTool({.name = "x", .arguments = Json::Value(Json::objectValue), .caller = {}}).failure.kind ==
        Failure::Kind::Malformed);
}
