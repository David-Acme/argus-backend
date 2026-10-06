#pragma once

#include <mcp/server.hxx>
#include <shared/vocabulary/tool-contracts.hxx>

#include <memory>
#include <vector>

[[nodiscard]] std::shared_ptr<argus::mcp::McpServer> coreToolServer(std::vector<tools::ToolDescriptor> descriptors);

[[nodiscard]] argus::mcp::ToolSpec appOpenSpec();
