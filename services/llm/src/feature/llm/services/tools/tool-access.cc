#include "tool-access.hxx"

#include <auth/capability.hxx>

#include <algorithm>

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

bool surfaceAllows(const ToolAudience& audience, const argus::mcp::ToolSpec& spec)
{
  switch (audience.surface) {
    case ConversationSurface::App:
      return true;
    case ConversationSurface::Camera:
      return role_access::inCameraWatchSet(spec.capability);
    case ConversationSurface::CameraVisitor:
      return std::ranges::find(role_access::kCameraVisitorCapabilities, spec.capability) !=
             role_access::kCameraVisitorCapabilities.end();
  }
  return false;
}

bool moduleInactive(const ToolAudience& audience, const argus::mcp::ToolSpec& spec)
{
  return holds(audience, spec) && !runnable(audience, spec);
}

}
