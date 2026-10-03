#include "tool-executor.hxx"

#include <auth/role-access.hxx>
#include <feature/llm/services/tools/tool-validator.hxx>

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

  if (!permits(*descriptor, role)) {
    result.output = "permission denied for tool: " + call.name;
    return result;
  }

  result = descriptor->handler(call);
  result.tool = call.name;
  return result;
}

bool ToolExecutor::permits(const tools::ToolDescriptor& descriptor,
                           UserRole role)
{
  return role_access::hasAccess({.role = role,
                                 .table = descriptor.accessTable,
                                 .perm = descriptor.accessPermission});
}

std::vector<const tools::ToolDescriptor*>
ToolExecutor::permittedTools(UserRole role) const
{
  std::vector<const tools::ToolDescriptor*> out;
  for (const auto& name : registry_.names()) {
    const auto* descriptor = registry_.find(name);
    if (descriptor != nullptr && permits(*descriptor, role))
      out.push_back(descriptor);
  }
  return out;
}
