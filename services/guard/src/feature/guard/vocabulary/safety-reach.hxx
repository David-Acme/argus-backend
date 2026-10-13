#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

enum class SafetyReach : uint8_t
{
  Others = 0,
  Contacts,
  Nobody,
  Unknown
};

inline std::string safetyReachToString(SafetyReach reach)
{
  switch (reach) {
    case SafetyReach::Others:
      return "others";
    case SafetyReach::Contacts:
      return "contacts";
    case SafetyReach::Nobody:
      return "nobody";
    case SafetyReach::Unknown:
      return "unknown";
  }
  return "unknown";
}

inline std::optional<SafetyReach> safetyReachFromString(std::string_view value)
{
  if (value == "others")
    return SafetyReach::Others;
  if (value == "contacts")
    return SafetyReach::Contacts;
  if (value == "nobody")
    return SafetyReach::Nobody;
  if (value == "unknown")
    return SafetyReach::Unknown;
  return std::nullopt;
}
