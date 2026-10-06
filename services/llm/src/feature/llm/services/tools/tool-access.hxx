#pragma once

#include <auth/module-snapshot.hxx>
#include <auth/user-role.hxx>
#include <mcp/tool.hxx>

struct ToolAudience
{
  UserRole role{UserRole::Unknown};
  ModuleSnapshot modules;
};

namespace tool_access
{

[[nodiscard]] bool holds(const ToolAudience& audience, const argus::mcp::ToolSpec& spec);

[[nodiscard]] bool runnable(const ToolAudience& audience, const argus::mcp::ToolSpec& spec);

[[nodiscard]] bool moduleInactive(const ToolAudience& audience, const argus::mcp::ToolSpec& spec);

}
