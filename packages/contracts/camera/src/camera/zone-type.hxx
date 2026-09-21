#pragma once

#include <cstdint>
#include <string>

enum class ZoneType : uint8_t
{
  Monitor = 0,
  Alert,
  Exclude
};

inline std::string zoneTypeToString(ZoneType t)
{
  switch (t) {
    case ZoneType::Alert:
      return "alert";
    case ZoneType::Exclude:
      return "exclude";
    default:
      return "monitor";
  }
}

inline ZoneType zoneTypeFromString(const std::string& s)
{
  if (s == "alert")
    return ZoneType::Alert;
  if (s == "exclude")
    return ZoneType::Exclude;
  return ZoneType::Monitor;
}
