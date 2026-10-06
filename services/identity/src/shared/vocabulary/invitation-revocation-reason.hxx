#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

enum class InvitationRevocationReason : std::uint8_t
{
  ModuleDisabled = 0
};

constexpr std::string_view invitationRevocationReasonToString(InvitationRevocationReason reason)
{
  switch (reason) {
  case InvitationRevocationReason::ModuleDisabled: return "module_disabled";
  }
  return "module_disabled";
}

constexpr std::optional<InvitationRevocationReason> invitationRevocationReasonFromString(std::string_view text)
{
  if (text == "module_disabled")
    return InvitationRevocationReason::ModuleDisabled;
  return std::nullopt;
}
