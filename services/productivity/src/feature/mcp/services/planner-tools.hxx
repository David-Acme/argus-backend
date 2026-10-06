#pragma once

#include <mcp/tool.hxx>

#include <drogon/utils/coroutine.h>

namespace productivity_tools
{

[[nodiscard]] drogon::Task<argus::mcp::ToolOutcome> createProject(argus::mcp::ToolInvocation invocation);

[[nodiscard]] drogon::Task<argus::mcp::ToolOutcome> listProjects(argus::mcp::ToolInvocation invocation);

[[nodiscard]] drogon::Task<argus::mcp::ToolOutcome> createTask(argus::mcp::ToolInvocation invocation);

[[nodiscard]] drogon::Task<argus::mcp::ToolOutcome> listTasks(argus::mcp::ToolInvocation invocation);

[[nodiscard]] drogon::Task<argus::mcp::ToolOutcome> completeTask(argus::mcp::ToolInvocation invocation);

}
