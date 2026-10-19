#pragma once

#include <feature/messenger/services/household-roster.hxx>
#include <feature/messenger/services/relay-service.hxx>
#include <mcp/confirmation.hxx>
#include <mcp/tool.hxx>
#include <shared/vocabulary/tool-contracts.hxx>

#include <memory>
#include <string_view>

inline constexpr std::string_view kRelayToolName = "message.relay";

struct RelayToolInput
{
  std::shared_ptr<const RelayService> service;
  std::shared_ptr<const HouseholdRoster> roster;
  std::shared_ptr<argus::mcp::ConfirmationLedger> ledger;
};

[[nodiscard]] argus::mcp::ToolSpec relayToolSpec();

[[nodiscard]] tools::ToolDescriptor relayTool(RelayToolInput input);
