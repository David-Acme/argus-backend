#pragma once

#include <auth/user-role.hxx>
#include <shared/vocabulary/tool-contracts.hxx>
#include <feature/llm/services/tools/tool-registry.hxx>
#include <string>
#include <vector>

class ToolExecutor
{
public:
  explicit ToolExecutor(ToolRegistry& registry) : registry_(registry) {}

  tools::ToolResult execute(const tools::ToolCall& call, UserRole role) const;

  static bool permits(const tools::ToolDescriptor& descriptor, UserRole role);

  std::vector<const tools::ToolDescriptor*> permittedTools(UserRole role) const;

private:
  ToolRegistry& registry_;
};
