#include "loop-tool.hxx"

#include <drogon/drogon.h>
#include <trantor/utils/Logger.h>

#include <exception>
#include <utility>

namespace argus::mcp
{

namespace
{
ToolOutcome fault(const std::string& name)
{
  ToolOutcome outcome;
  outcome.isError = true;
  outcome.text = "The tool failed: " + name;
  outcome.structured["code"] = "internal_error";
  return outcome;
}

struct Pending
{
  LoopHandler handler;
  ToolInvocation invocation;
  McpServer::Reply reply;
};

drogon::Task<void> run(Pending pending)
{
  ToolOutcome outcome;
  try {
    outcome = co_await pending.handler(pending.invocation);
  }
  catch (const std::exception& error) {
    LOG_WARN << "mcp: tool " << pending.invocation.name << " failed: " << error.what();
    outcome = fault(pending.invocation.name);
  }
  pending.reply(outcome);
}
}

McpServer::Handler onLoop(LoopTool tool)
{
  return [tool = std::move(tool)](const ToolInvocation& invocation, const McpServer::Reply& reply) {
    trantor::EventLoop* loop = tool.loop ? tool.loop() : drogon::app().getLoop();
    if (loop == nullptr) {
      reply(fault(invocation.name));
      return;
    }
    loop->queueInLoop([pending = Pending{.handler = tool.handler, .invocation = invocation, .reply = reply}] {
      drogon::async_run([pending]() -> drogon::Task<void> { co_await run(pending); });
    });
  };
}

}
