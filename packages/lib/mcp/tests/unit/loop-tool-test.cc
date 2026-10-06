#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "test-support.hxx"

#include <mcp/loop-tool.hxx>
#include <mcp/schema.hxx>

#include <trantor/net/EventLoopThread.h>

#include <atomic>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

using namespace argus::mcp;
using test_support::answer;
using test_support::must;
using test_support::resultOf;

namespace
{
struct LoopServer
{
  trantor::EventLoopThread thread;
  McpServer server{ServerIdentity{.name = "loop", .version = "1", .instructions = ""}};
  std::thread::id loopThread;

  LoopServer()
  {
    thread.run();
    trantor::EventLoop* loop = thread.getLoop();
    loop->runInLoop([this] { loopThread = std::this_thread::get_id(); });
    server.add({.name = "slow.echo",
                .title = "",
                .description = "",
                .inputSchema = schema::object({{.name = "text", .schema = schema::text(), .required = false}}),
                .annotations = {},
                .module = "core",
                .capability = ""},
               onLoop({.loop = [loop] { return loop; },
                       .handler = [this, loop](const ToolInvocation& invocation) -> drogon::Task<ToolOutcome> {
                         co_await drogon::sleepCoro(loop, 0.01);
                         ToolOutcome outcome;
                         outcome.text = invocation.arguments.get("text", "").asString();
                         outcome.structured["onLoop"] = std::this_thread::get_id() == loopThread;
                         co_return outcome;
                       }}));
    server.add({.name = "slow.boom",
                .title = "",
                .description = "",
                .inputSchema = schema::emptyObject(),
                .annotations = {},
                .module = "core",
                .capability = ""},
               onLoop({.loop = [loop] { return loop; },
                       .handler = [](const ToolInvocation&) -> drogon::Task<ToolOutcome> {
                         throw std::runtime_error("secret detail");
                         co_return ToolOutcome{};
                       }}));
    server.add({.name = "slow.detached",
                .title = "",
                .description = "",
                .inputSchema = schema::emptyObject(),
                .annotations = {},
                .module = "core",
                .capability = ""},
               onLoop({.loop = []() -> trantor::EventLoop* { return nullptr; },
                       .handler = [](const ToolInvocation&) -> drogon::Task<ToolOutcome> { co_return ToolOutcome{}; }}));
  }

  std::string call(const std::string& name, Json::Value arguments)
  {
    Json::Value info(Json::objectValue);
    Json::Value params(Json::objectValue);
    params["name"] = name;
    params["arguments"] = std::move(arguments);
    const Json::Value meta = requestMeta(info);
    for (const auto& key : meta.getMemberNames())
      params["_meta"][key] = meta[key];
    return server.handleBlocking(requestFrame({.hasId = true, .id = Json::Value(1), .method = "tools/call", .params = params}));
  }
};
}

TEST_CASE("a coroutine tool runs on the loop it was given, suspends, and answers")
{
  LoopServer loop;
  Json::Value arguments(Json::objectValue);
  arguments["text"] = "hola";
  const auto outcome = must(toolOutcomeFrom(resultOf(answer(loop.call("slow.echo", arguments)))));
  CHECK_FALSE(outcome.isError);
  CHECK(outcome.text == "hola");
  CHECK(outcome.structured["onLoop"].asBool());
}

TEST_CASE("a coroutine that throws becomes a tool error that hides the cause")
{
  LoopServer loop;
  const auto outcome = must(toolOutcomeFrom(resultOf(answer(loop.call("slow.boom", Json::Value(Json::objectValue))))));
  CHECK(outcome.isError);
  CHECK(outcome.text == "The tool failed: slow.boom");
  CHECK(outcome.text.find("secret detail") == std::string::npos);
  CHECK(outcome.structured["code"].asString() == "internal_error");
}

TEST_CASE("a tool with no loop to run on answers a tool error instead of waiting")
{
  LoopServer loop;
  const auto outcome = must(toolOutcomeFrom(resultOf(answer(loop.call("slow.detached", Json::Value(Json::objectValue))))));
  CHECK(outcome.isError);
  CHECK(outcome.text == "The tool failed: slow.detached");
}
