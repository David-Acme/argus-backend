#pragma once

#include <cstdint>
#include <string>

struct HealthMetrics
{
  double brightness{0.0};
  double blur{0.0};
  double sceneDiff{0.0};
};

enum class CameraHealthState
{
  Ok,
  Dark,
  Bright,
  Blurred,
  Moved,
  Unreachable,
};

struct HealthThresholds
{
  double dark{25.0};
  double bright{235.0};
  double blur{18.0};
  double sceneDiff{0.35};
};

struct CameraHealthEvent
{
  int64_t cameraId{0};
  std::string cameraName;
  CameraHealthState status{CameraHealthState::Ok};
  HealthMetrics metrics;
  int64_t detectedAtMs{0};
};

namespace health_monitor
{
CameraHealthState classify(const HealthMetrics& metrics,
                      const HealthThresholds& thresholds);
std::string statusName(CameraHealthState status);
} // namespace health_monitor

// Sink for health transitions; NATS is the production implementation.
class IHealthEventSink
{
public:
  virtual ~IHealthEventSink() = default;
  virtual bool publish(const CameraHealthEvent& event) = 0;
};
