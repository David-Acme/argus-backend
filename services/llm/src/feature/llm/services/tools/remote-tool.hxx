#pragma once

#include <mcp/client.hxx>
#include <shared/vocabulary/tool-contracts.hxx>

#include <memory>
#include <string>
#include <string_view>

struct RemoteToolInput
{
  std::string provider;
  std::shared_ptr<argus::mcp::McpClient> client;
  argus::mcp::ToolSpec spec;
};

[[nodiscard]] tools::ToolHandle remoteTool(const RemoteToolInput& input);

[[nodiscard]] std::string_view appNotConnected(std::string_view lang);

[[nodiscard]] std::string_view toolUnreachable(std::string_view lang);
