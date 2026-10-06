#pragma once

#include <feature/mcp/services/module-desk.hxx>
#include <mcp/server.hxx>

#include <functional>
#include <memory>
#include <trantor/net/EventLoop.h>

struct ModuleToolsInput
{
  std::shared_ptr<ModuleDesk> desk;
  std::function<trantor::EventLoop*()> loop;
};

[[nodiscard]] std::shared_ptr<argus::mcp::McpServer> moduleToolServer(const ModuleToolsInput& input);
