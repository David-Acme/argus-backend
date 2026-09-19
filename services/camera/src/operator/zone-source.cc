#include <operator/zone-source.hxx>

#include <shared/utils/json-util/json-util.hxx>

#include <json/value.h>
#include <utility>

StaticZoneSource::StaticZoneSource(std::vector<OperatorZone> zones)
    : zones_(std::move(zones))
{
}

std::vector<OperatorZone> StaticZoneSource::forCamera(int64_t cameraId)
{
  std::vector<OperatorZone> zones;
  for (const auto& zone : zones_) {
    if (zone.cameraId == cameraId)
      zones.push_back(zone);
  }
  return zones;
}

std::vector<std::pair<double, double>> parseZonePoints(const std::string& text)
{
  std::vector<std::pair<double, double>> points;
  const Json::Value json = json_util::fromString(text);
  if (!json.isArray())
    return points;
  for (const auto& point : json) {
    if (point.isObject() && point.isMember("x") && point.isMember("y"))
      points.emplace_back(point["x"].asDouble(), point["y"].asDouble());
  }
  return points;
}
