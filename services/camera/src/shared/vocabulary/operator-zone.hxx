#pragma once

#include <camera/zone-type.hxx>

#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

struct OperatorZone
{
  int64_t cameraId{0};
  std::string name;
  ZoneType kind{ZoneType::Monitor};
  std::vector<std::pair<double, double>> points;
};

namespace operator_zone
{
inline std::optional<ZoneType> kindOf(const std::string& text)
{
  const ZoneType kind = zoneTypeFromString(text);
  if (zoneTypeToString(kind) != text)
    return std::nullopt;
  return kind;
}
}
