#include <feature/operator/zone-source.hxx>

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
