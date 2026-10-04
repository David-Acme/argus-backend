#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

enum class EnvironmentKind : uint8_t
{
  Home = 0,
  Office,
  Commercial,
  Restaurant,
  Warehouse,
  Outdoor
};

inline constexpr std::array<std::pair<EnvironmentKind, std::string_view>, 6>
    kEnvironmentKindNames{{{EnvironmentKind::Home, "home"},
                           {EnvironmentKind::Office, "office"},
                           {EnvironmentKind::Commercial, "commercial"},
                           {EnvironmentKind::Restaurant, "restaurant"},
                           {EnvironmentKind::Warehouse, "warehouse"},
                           {EnvironmentKind::Outdoor, "outdoor"}}};

inline std::string environmentKindToString(EnvironmentKind kind)
{
  for (const auto& [value, name] : kEnvironmentKindNames) {
    if (value == kind)
      return std::string(name);
  }
  return "home";
}

inline std::optional<EnvironmentKind>
environmentKindFromString(std::string_view value)
{
  for (const auto& [kind, name] : kEnvironmentKindNames) {
    if (name == value)
      return kind;
  }
  return std::nullopt;
}
