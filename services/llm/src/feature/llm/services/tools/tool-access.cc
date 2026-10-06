#include "tool-access.hxx"

#include <auth/capability.hxx>

namespace tool_access
{

bool holds(const ToolAudience& audience, const argus::mcp::ToolSpec& spec)
{
  if (spec.capability.empty())
    return false;
  return role_access::hasCapability({.role = audience.role, .modules = ModuleSnapshot{}, .capability = spec.capability});
}

bool runnable(const ToolAudience& audience, const argus::mcp::ToolSpec& spec)
{
  if (spec.capability.empty())
    return false;
  return role_access::hasCapability(
      {.role = audience.role, .modules = audience.modules, .capability = spec.capability});
}

bool moduleInactive(const ToolAudience& audience, const argus::mcp::ToolSpec& spec)
{
  return holds(audience, spec) && !runnable(audience, spec);
}

}
