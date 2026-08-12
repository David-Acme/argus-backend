#pragma once

#include <shared/contracts/tool-contracts.hxx>
#include <shared/services/tools/tool-registry.hxx>
#include <string>

// resolve -> validate -> role_access::hasAccess -> handler (AGENTS.md §7:
// the central role check is the single source of truth).
class ToolExecutor
{
public:
  explicit ToolExecutor(ToolRegistry& registry) : registry_(registry) {}

  tools::ToolResult execute(const tools::ToolCall& call, UserRole role) const;

private:
  ToolRegistry& registry_;
};
