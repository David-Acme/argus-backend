#pragma once

#include <auth/user-role.hxx>
#include <llm/tool-contracts.hxx>
#include <shared/services/tools/tool-registry.hxx>
#include <string>

class ToolExecutor
{
public:
  explicit ToolExecutor(ToolRegistry& registry) : registry_(registry) {}

  tools::ToolResult execute(const tools::ToolCall& call, UserRole role) const;

private:
  ToolRegistry& registry_;
};
