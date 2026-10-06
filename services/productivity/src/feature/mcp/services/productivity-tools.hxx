#pragma once

#include <mcp/server.hxx>

#include <functional>
#include <memory>
#include <trantor/net/EventLoop.h>

struct ProductivityToolsInput
{
  std::function<trantor::EventLoop*()> loop;
};

[[nodiscard]] std::shared_ptr<argus::mcp::McpServer> productivityToolServer(const ProductivityToolsInput& input);
