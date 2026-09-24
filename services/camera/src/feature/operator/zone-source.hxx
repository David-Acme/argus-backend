#pragma once

#include <feature/operator/event-intelligence.hxx>

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

class IZoneSource
{
public:
  virtual ~IZoneSource() = default;
  virtual std::vector<OperatorZone> forCamera(int64_t cameraId) = 0;
};

class StaticZoneSource final : public IZoneSource
{
public:
  explicit StaticZoneSource(std::vector<OperatorZone> zones);

  std::vector<OperatorZone> forCamera(int64_t cameraId) override;

private:
  std::vector<OperatorZone> zones_;
};

std::vector<std::pair<double, double>> parseZonePoints(const std::string& text);
