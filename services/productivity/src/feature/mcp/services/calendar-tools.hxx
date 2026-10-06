#pragma once

#include <mcp/confirmation.hxx>
#include <mcp/tool.hxx>

#include <drogon/utils/coroutine.h>
#include <memory>

namespace productivity_tools
{

struct CancelRequest
{
  argus::mcp::ToolInvocation invocation;
  std::shared_ptr<argus::mcp::ConfirmationLedger> ledger;
};

[[nodiscard]] drogon::Task<argus::mcp::ToolOutcome> createEvent(argus::mcp::ToolInvocation invocation);

[[nodiscard]] drogon::Task<argus::mcp::ToolOutcome> listEvents(argus::mcp::ToolInvocation invocation);

[[nodiscard]] drogon::Task<argus::mcp::ToolOutcome> cancelEvent(CancelRequest request);

}
