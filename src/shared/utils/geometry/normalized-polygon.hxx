#pragma once

#include <json/json.h>
#include <optional>
#include <string>

namespace geometry
{

// Zone polygons travel as normalized [0..1] coordinates so the same zone maps
// onto any resolution the camera or the client happens to render at. The
// canonical form is a compact JSON array, which is what the `zone.points`
// column stores.
inline constexpr int kMinPolygonPoints = 3;
inline constexpr int kMaxPolygonPoints = 64;

inline std::optional<std::string>
serializeNormalizedPolygon(const Json::Value& points)
{
  if (!points.isArray())
    return std::nullopt;
  const auto count = static_cast<int>(points.size());
  if (count < kMinPolygonPoints || count > kMaxPolygonPoints)
    return std::nullopt;

  Json::Value out(Json::arrayValue);
  for (const auto& point : points) {
    if (!point.isObject() || !point.isMember("x") || !point.isMember("y"))
      return std::nullopt;
    if (!point["x"].isNumeric() || !point["y"].isNumeric())
      return std::nullopt;

    const double x = point["x"].asDouble();
    const double y = point["y"].asDouble();
    if (x < 0.0 || x > 1.0 || y < 0.0 || y > 1.0)
      return std::nullopt;

    Json::Value entry;
    entry["x"] = x;
    entry["y"] = y;
    out.append(entry);
  }

  Json::StreamWriterBuilder builder;
  builder["indentation"] = "";
  // Six decimals is sub-pixel even on a 4K frame, and it keeps the stored
  // string short instead of spelling out the full double (0.10000000000000001).
  builder["precision"] = 6;
  builder["precisionType"] = "decimal";
  return Json::writeString(builder, out);
}

} // namespace geometry
