#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <grpc/grpc-client-base.hxx>
#include <mcp/mcp-rpc.hxx>
#include <mcp/schema.hxx>

#include <grpcpp/grpcpp.h>

#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace
{
namespace wire = argus::mcp::v1;
using namespace argus::mcp;

template <class T>
T must(std::optional<T> value)
{
  REQUIRE(value.has_value());
  return std::move(value).value_or(T{});
}

RpcResponse answer(const std::string& frame)
{
  return must(parseResponse(frame));
}

Json::Value resultOf(const RpcResponse& response)
{
  REQUIRE(response.result.has_value());
  return response.result.value_or(Json::Value());
}

RpcError errorOf(const RpcResponse& response)
{
  REQUIRE(response.error.has_value());
  return response.error.value_or(RpcError{});
}

constexpr const char* kLlmSecret = "llm-mcp-secret-0123456789abcdef";
constexpr const char* kOtherSecret = "other-mcp-secret-0123456789abcd";

std::shared_ptr<const McpServer> sampleServer(std::vector<std::jthread>& workers)
{
  auto server = std::make_shared<McpServer>(ServerIdentity{.name = "argus-test", .version = "1", .instructions = ""});
  server->addSync({.name = "memory.recall",
                   .title = "",
                   .description = "Recalls",
                   .inputSchema = schema::object({{.name = "query", .schema = schema::text(), .required = true}}),
                   .annotations = {},
                   .module = "core",
                   .capability = "memory.manage"},
                  [](const ToolInvocation& invocation) {
                    ToolOutcome outcome;
                    outcome.text = "recalled " + invocation.arguments["query"].asString();
                    return outcome;
                  });
  server->add({.name = "slow",
               .title = "",
               .description = "Answers from another thread",
               .inputSchema = schema::emptyObject(),
               .annotations = {},
               .module = "core",
               .capability = ""},
              [&workers](const ToolInvocation&, McpServer::Reply reply) {
                workers.emplace_back([reply = std::move(reply)] {
                  ToolOutcome outcome;
                  outcome.text = "late";
                  reply(outcome);
                });
              });
  return server;
}

std::shared_ptr<const argus::client::FleetCallerGate> gateWith(
    std::vector<std::pair<std::string, std::string>> pairs)
{
  return std::make_shared<const argus::client::FleetCallerGate>(
      argus::client::FleetGateConfig{.expectedCallers = {}, .callerPairs = std::move(pairs), .legacySecret = {}, .onFirstLegacy = {}});
}

struct Sending
{
  std::string payload;
  std::string secret;
};

struct Harness
{
  std::vector<std::jthread> workers;
  McpRpcService service;
  std::unique_ptr<grpc::Server> server;
  std::unique_ptr<wire::Mcp::Stub> stub;

  Harness()
      : service({.server = sampleServer(workers),
                 .gate = gateWith({{"llm", kLlmSecret}, {"voice", kOtherSecret}}),
                 .callers = {"llm"}})
  {
    grpc::ServerBuilder builder;
    builder.RegisterService(&service);
    server = builder.BuildAndStart();
    stub = wire::Mcp::NewStub(server->InProcessChannel({}));
  }

  ~Harness() { server->Shutdown(); }

  grpc::Status send(const Sending& sending, wire::Frame& reply)
  {
    grpc::ClientContext context;
    if (!sending.secret.empty())
      context.AddMetadata(argus::client::kCallerCredentialKey, sending.secret);
    wire::Frame request;
    request.set_payload(sending.payload);
    return stub->Rpc(&context, request, &reply);
  }
};

std::string listFrame()
{
  Json::Value info(Json::objectValue);
  info["name"] = "test";
  RpcRequest request;
  request.hasId = true;
  request.id = Json::Value(1);
  request.method = "tools/list";
  request.params["_meta"] = requestMeta(info);
  return requestFrame(request);
}

struct CallFrame
{
  std::string name;
  std::string query;
};

std::string callFrame(const CallFrame& call)
{
  Json::Value info(Json::objectValue);
  info["name"] = "test";
  RpcRequest request;
  request.hasId = true;
  request.id = Json::Value(2);
  request.method = "tools/call";
  request.params["_meta"] = requestMeta(info);
  request.params["name"] = call.name;
  request.params["arguments"] = Json::Value(Json::objectValue);
  if (!call.query.empty())
    request.params["arguments"]["query"] = call.query;
  return requestFrame(request);
}
}

TEST_CASE("the paired llm credential lists and calls tools through the wire")
{
  Harness harness;
  wire::Frame reply;
  REQUIRE(harness.send({.payload = listFrame(), .secret = kLlmSecret}, reply).ok());
  CHECK(resultOf(answer(reply.payload()))["tools"].size() == 2);

  wire::Frame called;
  REQUIRE(harness.send({.payload = callFrame({.name = "memory.recall", .query = "dentista"}), .secret = kLlmSecret}, called).ok());
  CHECK(must(toolOutcomeFrom(resultOf(answer(called.payload())))).text == "recalled dentista");
}

TEST_CASE("a handler that answers from another thread still finishes the call")
{
  Harness harness;
  wire::Frame reply;
  REQUIRE(harness.send({.payload = callFrame({.name = "slow", .query = ""}), .secret = kLlmSecret}, reply).ok());
  CHECK(must(toolOutcomeFrom(resultOf(answer(reply.payload())))).text == "late");
}

TEST_CASE("a call with no credential or a wrong one is unauthenticated and never reaches the server")
{
  Harness harness;
  wire::Frame reply;
  CHECK(harness.send({.payload = listFrame(), .secret = ""}, reply).error_code() == grpc::StatusCode::UNAUTHENTICATED);
  CHECK(harness.send({.payload = listFrame(), .secret = "not-a-secret"}, reply).error_code() ==
        grpc::StatusCode::UNAUTHENTICATED);
  CHECK(reply.payload().empty());
}

TEST_CASE("a paired fleet caller that is not llm is forbidden")
{
  Harness harness;
  wire::Frame reply;
  CHECK(harness.send({.payload = listFrame(), .secret = kOtherSecret}, reply).error_code() ==
        grpc::StatusCode::PERMISSION_DENIED);
  CHECK(reply.payload().empty());
}

TEST_CASE("a frame over the size bound is refused before it is parsed")
{
  Harness harness;
  wire::Frame reply;
  const auto status = harness.send({.payload = std::string(kMaxFrameBytes + 1, 'x'), .secret = kLlmSecret}, reply);
  CHECK(status.error_code() == grpc::StatusCode::INVALID_ARGUMENT);
}

TEST_CASE("garbage inside the admitted frame is a JSON-RPC parse error, not a transport error")
{
  Harness harness;
  wire::Frame reply;
  REQUIRE(harness.send({.payload = "{not json", .secret = kLlmSecret}, reply).ok());
  CHECK(errorOf(answer(reply.payload())).code == codeOf(ErrorCode::ParseError));
}

TEST_CASE("a notification is answered with an empty frame")
{
  Harness harness;
  wire::Frame reply;
  REQUIRE(harness.send({.payload = R"({"jsonrpc":"2.0","method":"notifications/x"})", .secret = kLlmSecret}, reply).ok());
  CHECK(reply.payload().empty());
}

TEST_CASE("a surface is never built open, without a server or without an admitted caller")
{
  std::vector<std::jthread> workers;
  const auto server = sampleServer(workers);
  CHECK_THROWS_AS(McpRpcService({.server = server, .gate = gateWith({}), .callers = {"llm"}}), std::invalid_argument);
  CHECK_THROWS_AS(McpRpcService({.server = server, .gate = gateWith({{"llm", "CHANGE_ME"}}), .callers = {"llm"}}),
                  std::invalid_argument);
  CHECK_THROWS_AS(McpRpcService({.server = server, .gate = nullptr, .callers = {"llm"}}), std::invalid_argument);
  CHECK_THROWS_AS(McpRpcService({.server = nullptr, .gate = gateWith({{"llm", kLlmSecret}}), .callers = {"llm"}}),
                  std::invalid_argument);
  CHECK_THROWS_AS(McpRpcService({.server = server, .gate = gateWith({{"llm", kLlmSecret}}), .callers = {}}),
                  std::invalid_argument);
}
