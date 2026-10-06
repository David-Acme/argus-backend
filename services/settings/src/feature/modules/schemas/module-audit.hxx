#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string_view>

enum class ModuleAuditAction : std::uint8_t
{
  Adopted,
  InstallRequested,
  Enabled,
  Disabled,
  RolledBack,
  Paused,
  Resumed,
  Cancelled,
  Failed,
  UninstallRequested,
  Removed,
  Purged
};

inline constexpr std::array<std::string_view, 12> kModuleAuditActionNames{
    "adopted", "install_requested", "enabled", "disabled",            "rolled_back", "paused",
    "resumed", "cancelled",         "failed",  "uninstall_requested", "removed",     "purged"};

constexpr std::string_view moduleAuditActionToString(ModuleAuditAction action)
{
  return kModuleAuditActionNames.at(static_cast<std::size_t>(action));
}

constexpr std::optional<ModuleAuditAction> moduleAuditActionFromString(std::string_view text)
{
  for (std::size_t index = 0; index < kModuleAuditActionNames.size(); ++index)
    if (kModuleAuditActionNames.at(index) == text)
      return static_cast<ModuleAuditAction>(index);
  return std::nullopt;
}

constexpr bool changesEnabledSet(ModuleAuditAction action)
{
  return action == ModuleAuditAction::Adopted || action == ModuleAuditAction::Enabled ||
         action == ModuleAuditAction::Disabled || action == ModuleAuditAction::RolledBack ||
         action == ModuleAuditAction::Removed || action == ModuleAuditAction::Purged;
}
