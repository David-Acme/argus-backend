#include "guard-radar-source.hxx"

#include <utility>

std::vector<RadarFix> radarFixesOf(const std::vector<argus::guard::v1::SensorTargets>& sensors)
{
  std::vector<RadarFix> fixes;
  fixes.reserve(sensors.size());
  for (const argus::guard::v1::SensorTargets& sensor : sensors) {
    RadarFix fix{.sensorId = sensor.sensor_id(),
                 .panOffsetDeg = sensor.pan_offset_deg(),
                 .targets = {},
                 .atMs = sensor.at_ms()};
    fix.targets.reserve(static_cast<size_t>(sensor.targets_size()));
    for (const argus::guard::v1::RadarTarget& target : sensor.targets())
      fix.targets.push_back({.xMm = target.x_mm(), .yMm = target.y_mm()});
    fixes.push_back(std::move(fix));
  }
  return fixes;
}

GuardRadarSource::GuardRadarSource(std::shared_ptr<const GuardPresenceClient> client) : client_(std::move(client)) {}

std::vector<RadarFix> GuardRadarSource::fixes(int64_t cameraId) const
{
  if (!client_)
    return {};
  const auto sensors = client_->sensorTargets(cameraId);
  if (!sensors)
    return {};
  return radarFixesOf(*sensors);
}
