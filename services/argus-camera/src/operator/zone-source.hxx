#pragma once

#include <operator/event-intelligence.hxx>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

// Supplies zone polygons to the operator; the camera-db provider is the production one.
class IZoneSource
{
public:
  virtual ~IZoneSource() = default;
  virtual std::vector<OperatorZone> forCamera(int64_t cameraId) = 0;
};

// Fixed zones (config fallback) filtered by camera.
class StaticZoneSource final : public IZoneSource
{
public:
  explicit StaticZoneSource(std::vector<OperatorZone> zones);

  std::vector<OperatorZone> forCamera(int64_t cameraId) override;

private:
  std::vector<OperatorZone> zones_;
};

// Parses the zone table's [{"x":..,"y":..}] JSON into normalized points.
std::vector<std::pair<double, double>> parseZonePoints(const std::string& text);
