#include "tool-gate.hxx"

#include <auth/capability.hxx>
#include <auth/module-gate.hxx>

#include <string>

namespace tool_gate
{

argus::mcp::McpServer::Gate capabilities()
{
  return [](const argus::mcp::ToolInvocation& invocation,
            const argus::mcp::ToolSpec& spec) -> std::optional<argus::mcp::Refusal> {
    const UserRole role = userRoleFromString(invocation.caller.role);
    const ModuleSnapshot everything;
    if (spec.capability.empty() ||
        !role_access::hasCapability({.role = role, .modules = everything, .capability = spec.capability}))
      return argus::mcp::Refusal{.code = "forbidden", .message = "permission denied for tool: " + spec.name};
    const ModuleSnapshot current = moduleGate().snapshot();
    if (!role_access::hasCapability({.role = role, .modules = current, .capability = spec.capability}))
      return argus::mcp::Refusal{.code = "module_inactive", .message = "The " + spec.module + " module is turned off"};
    return std::nullopt;
  };
}

}
