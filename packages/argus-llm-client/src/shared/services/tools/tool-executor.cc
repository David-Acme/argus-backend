#include "tool-executor.hxx"

#include <shared/access/role-access.hxx>
#include <shared/services/tools/tool-validator.hxx>

tools::ToolResult ToolExecutor::execute(const tools::ToolCall& call,
                                        UserRole role) const
{
  tools::ToolResult result;
  result.tool = call.name;

  const auto* descriptor = registry_.find(call.name);
  if (!descriptor) {
    result.output = "unknown tool: " + call.name;
    return result;
  }

  const auto error = tools::validateArguments(*descriptor, call);
  if (error) {
    result.output = *error;
    return result;
  }

  if (!role_access::hasAccess({.role = role,
                               .table = descriptor->accessTable,
                               .perm = descriptor->accessPermission})) {
    result.output = "permission denied for tool: " + call.name;
    return result;
  }

  result = descriptor->handler(call);
  result.tool = call.name;
  return result;
}
