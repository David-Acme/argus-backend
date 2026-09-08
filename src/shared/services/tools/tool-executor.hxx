#pragma once

#include <shared/contracts/tool-contracts.hxx>
#include <shared/services/tools/tool-registry.hxx>
#include <string>

// Tool call pipeline: resolve -> validate -> role_access::hasAccess -> handler.
class ToolExecutor
{
public:
  explicit ToolExecutor(ToolRegistry& registry) : registry_(registry) {}

  tools::ToolResult execute(const tools::ToolCall& call, UserRole role) const;

private:
  ToolRegistry& registry_;
};
