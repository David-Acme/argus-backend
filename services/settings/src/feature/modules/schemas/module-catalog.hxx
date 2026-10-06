#pragma once

#include <settings/component-vocabulary.hxx>

#include <algorithm>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class ModuleKind : std::uint8_t
{
  Core,
  Available,
  ComingSoon
};

constexpr std::string_view moduleKindToString(ModuleKind kind)
{
  switch (kind) {
  case ModuleKind::Core: return "core";
  case ModuleKind::Available: return "available";
  case ModuleKind::ComingSoon: return "coming_soon";
  }
  return "available";
}

constexpr std::optional<ModuleKind> moduleKindFromString(std::string_view text)
{
  if (text == "core")
    return ModuleKind::Core;
  if (text == "available")
    return ModuleKind::Available;
  if (text == "coming_soon")
    return ModuleKind::ComingSoon;
  return std::nullopt;
}

struct LocalizedText
{
  std::string es;
  std::string en;

  [[nodiscard]] const std::string& in(std::string_view lang) const { return lang == "en" ? en : es; }
};

struct GettingStartedItem
{
  std::string id;
  LocalizedText title;
  std::string route;
};

struct CatalogComponent
{
  ComponentSpec spec;
  std::string owner;
  std::int64_t ramMb{0};
};

struct HardwareRequirement
{
  std::int64_t minRamMb{0};
  std::int64_t recommendedRamMb{0};
  std::vector<std::string> requiredCpu;
  std::vector<std::string> recommendedCpu;
  bool recommendedGpu{false};
};

struct CatalogModule
{
  std::string id;
  ModuleKind kind{ModuleKind::Available};
  LocalizedText name;
  LocalizedText summary;
  std::vector<std::string> required;
  std::vector<std::string> components;
  std::vector<std::string> gates;
  std::vector<std::string> dataOwners;
  HardwareRequirement hardware;
  std::vector<GettingStartedItem> gettingStarted;
  std::vector<std::string> roles;
  ModuleIntroLocalized intro;
};

struct ModuleCatalog
{
  std::vector<CatalogModule> modules;
  std::vector<CatalogComponent> components;

  [[nodiscard]] const CatalogModule* module(std::string_view id) const
  {
    const auto found = std::ranges::find(modules, id, &CatalogModule::id);
    return found == modules.end() ? nullptr : &*found;
  }

  [[nodiscard]] const CatalogComponent* component(std::string_view id) const
  {
    const auto found =
        std::ranges::find_if(components, [id](const CatalogComponent& entry) { return entry.spec.id == id; });
    return found == components.end() ? nullptr : &*found;
  }

  [[nodiscard]] std::int64_t sizeBytes(const CatalogModule& entry) const
  {
    std::int64_t total = 0;
    for (const auto& id : entry.components)
      if (const auto* found = component(id))
        total += found->spec.totalBytes();
    return total;
  }
};
