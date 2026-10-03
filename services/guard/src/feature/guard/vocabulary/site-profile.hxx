#pragma once

#include <cstdint>
#include <optional>
#include <string>

enum class SiteProfile : uint8_t
{
  Home = 0,
  Office,
  Commercial
};

inline std::string siteProfileToString(SiteProfile profile)
{
  switch (profile) {
    case SiteProfile::Home:
      return "home";
    case SiteProfile::Office:
      return "office";
    case SiteProfile::Commercial:
      return "commercial";
  }
  return "home";
}

inline std::optional<SiteProfile> siteProfileFromString(const std::string& value)
{
  if (value == "home")
    return SiteProfile::Home;
  if (value == "office")
    return SiteProfile::Office;
  if (value == "commercial")
    return SiteProfile::Commercial;
  return std::nullopt;
}
