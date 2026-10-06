#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

enum class ModuleLifecycle : std::uint8_t
{
  NotInstalled,
  Active,
  Disabled,
  UninstalledDataKept
};

constexpr std::string_view moduleLifecycleToString(ModuleLifecycle lifecycle)
{
  switch (lifecycle) {
  case ModuleLifecycle::NotInstalled: return "not_installed";
  case ModuleLifecycle::Active: return "active";
  case ModuleLifecycle::Disabled: return "disabled";
  case ModuleLifecycle::UninstalledDataKept: return "uninstalled_data_kept";
  }
  return "not_installed";
}

constexpr std::optional<ModuleLifecycle> moduleLifecycleFromString(std::string_view text)
{
  for (const auto lifecycle : {ModuleLifecycle::NotInstalled, ModuleLifecycle::Active, ModuleLifecycle::Disabled,
                               ModuleLifecycle::UninstalledDataKept})
    if (moduleLifecycleToString(lifecycle) == text)
      return lifecycle;
  return std::nullopt;
}

struct ModuleStateSchema
{
  std::string moduleId;
  ModuleLifecycle lifecycle{ModuleLifecycle::NotInstalled};
  std::int64_t dataPurgedAt{0};
  std::int64_t updatedAt{0};
};
