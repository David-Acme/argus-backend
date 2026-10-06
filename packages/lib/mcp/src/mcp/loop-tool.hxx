#pragma once

#include <mcp/server.hxx>

#include <drogon/utils/coroutine.h>
#include <trantor/net/EventLoop.h>

#include <functional>

namespace argus::mcp
{

using LoopHandler = std::function<drogon::Task<ToolOutcome>(const ToolInvocation&)>;

struct LoopTool
{
  std::function<trantor::EventLoop*()> loop;
  LoopHandler handler;
};

[[nodiscard]] McpServer::Handler onLoop(LoopTool tool);

}
