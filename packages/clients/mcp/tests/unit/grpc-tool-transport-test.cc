#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <mcp/client.hxx>
#include <mcp/grpc-tool-transport.hxx>
#include <mcp/mcp-rpc.hxx>
#include <mcp/schema.hxx>

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{
using namespace argus::mcp;

template <class T>
T valueOf(const Response<T>& response)
{
  REQUIRE(response.ok());
  return response.valueOrDefault();
}

constexpr const char* kSecret = "llm-mcp-secret-0123456789abcdef";

struct LiveServer
{
  std::shared_ptr<McpServer> tools;
  std::unique_ptr<McpRpcService> service;
  std::unique_ptr<grpc::Server> server;
  int port{0};

  explicit LiveServer(const std::string& secret)
      : tools(std::make_shared<McpServer>(ServerIdentity{.name = "argus-test", .version = "3", .instructions = ""}))
  {
    tools->addSync({.name = "memory.recall",
                    .title = "",
                    .description = "Recalls",
                    .inputSchema = schema::object({{.name = "query", .schema = schema::text(), .required = true}}),
                    .annotations = {.readOnly = true},
                    .module = "core",
                    .capability = "memory.manage"},
                   [](const ToolInvocation& invocation) {
                     ToolOutcome outcome;
                     outcome.text = "recalled " + invocation.arguments["query"].asString() + " for " +
                                    std::to_string(invocation.caller.userId);
                     return outcome;
                   });
    service = std::make_unique<McpRpcService>(McpRpcInput{
        .server = tools,
        .gate = std::make_shared<const argus::client::FleetCallerGate>(argus::client::FleetGateConfig{
            .expectedCallers = {}, .callerPairs = {{"llm", secret}}, .legacySecret = {}, .onFirstLegacy = {}}),
        .callers = {"llm"}});
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(service.get());
    server = builder.BuildAndStart();
  }

  ~LiveServer() { server->Shutdown(); }

  [[nodiscard]] std::string target() const { return "127.0.0.1:" + std::to_string(port); }
};

McpClient clientFor(const std::string& target, const std::string& credential)
{
  return McpClient(std::make_shared<GrpcToolTransport>(ToolEndpoint{.target = target,
                                                                    .credential = credential,
                                                                    .timeout = std::chrono::milliseconds(3000)}),
                   {.name = "argus-llm", .version = "1"});
}
}

TEST_CASE("a client discovers, lists and calls tools over a live gRPC listener")
{
  LiveServer live(kSecret);
  const McpClient client = clientFor(live.target(), kSecret);

  const auto described = valueOf(client.discover());
  CHECK(described.name == "argus-test");
  CHECK(described.version == "3");

  const auto listed = valueOf(client.listTools());
  REQUIRE(listed.tools.size() == 1);
  CHECK(listed.tools[0].capability == "memory.manage");

  Json::Value arguments(Json::objectValue);
  arguments["query"] = "dentista";
  const auto called = valueOf(client.callTool(
      {.name = "memory.recall",
       .arguments = arguments,
       .caller = {.userId = 12, .role = "resident", .lang = "es", .sessionId = "", .utterance = "", .decided = false}}));
  CHECK(called.text == "recalled dentista for 12");
}

TEST_CASE("a wrong credential is a transport failure the caller can see, not a tool answer")
{
  LiveServer live(kSecret);
  const McpClient client = clientFor(live.target(), "another-credential-0123456789abcdef");
  const auto listed = client.listTools();
  CHECK_FALSE(listed.ok());
  CHECK(listed.failure.kind == Failure::Kind::Transport);
}

TEST_CASE("a listener that is not there is a transport failure within the deadline")
{
  const McpClient client = clientFor("127.0.0.1:1", kSecret);
  const auto started = std::chrono::steady_clock::now();
  const auto listed = client.listTools();
  CHECK_FALSE(listed.ok());
  CHECK(listed.failure.kind == Failure::Kind::Transport);
  CHECK(std::chrono::steady_clock::now() - started < std::chrono::seconds(10));
}

TEST_CASE("an endpoint without a target, a credential or a sane timeout is refused before dialling")
{
  CHECK_THROWS_AS(GrpcToolTransport(ToolEndpoint{.target = "", .credential = kSecret, .timeout = std::chrono::milliseconds(1000)}),
                  std::invalid_argument);
  CHECK_THROWS_AS(GrpcToolTransport(ToolEndpoint{.target = "127.0.0.1:1", .credential = "", .timeout = std::chrono::milliseconds(1000)}),
                  std::invalid_argument);
  CHECK_THROWS_AS(GrpcToolTransport(ToolEndpoint{.target = "127.0.0.1:1", .credential = kSecret, .timeout = std::chrono::milliseconds(0)}),
                  std::invalid_argument);
  CHECK_THROWS_AS(GrpcToolTransport(ToolEndpoint{.target = "127.0.0.1:1", .credential = kSecret, .timeout = std::chrono::minutes(3)}),
                  std::invalid_argument);
}
