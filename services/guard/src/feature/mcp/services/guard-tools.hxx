#pragma once

#include <mcp/server.hxx>

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <functional>
#include <memory>
#include <string>
#include <trantor/net/EventLoop.h>
#include <vector>

struct EnvironmentChoice
{
  int64_t id{0};
  std::string name;
};

using EnvironmentCatalog = std::function<drogon::Task<std::vector<EnvironmentChoice>>()>;

struct GuardToolsInput
{
  EnvironmentCatalog catalog;
  std::function<trantor::EventLoop*()> loop;
};

[[nodiscard]] std::shared_ptr<argus::mcp::McpServer> guardToolServer(const GuardToolsInput& input);
