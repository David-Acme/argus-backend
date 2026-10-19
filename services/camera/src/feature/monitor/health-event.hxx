#pragma once

#include <camera/camera-privacy-state.hxx>
#include <shared/vocabulary/health-thresholds.hxx>

#include <cstdint>
#include <string>
#include <string_view>

struct HealthMetrics
{
  double brightness{0.0};
  double blur{0.0};
  double sceneDiff{0.0};
  bool paused{false};
  bool homeDrift{false};
};

enum class CameraHealthState
{
  Ok,
  Dark,
  Bright,
  Blurred,
  Moved,
  Unreachable,
  Covered,
};

struct CameraHealthEvent
{
  int64_t cameraId{0};
  std::string cameraName;
  CameraHealthState status{CameraHealthState::Ok};
  HealthMetrics metrics;
  int64_t detectedAtMs{0};
};

struct CameraPrivacyEvent
{
  int64_t cameraId{0};
  std::string cameraName{};
  CameraPrivacyState state{CameraPrivacyState::Unknown};
  bool obeyed{false};
  int64_t detectedAtMs{0};
};

namespace health_monitor
{
CameraHealthState classify(const HealthMetrics& metrics,
                      const HealthThresholds& thresholds);
std::string_view statusName(CameraHealthState status);
}

class IHealthEventSink
{
public:
  virtual ~IHealthEventSink() = default;
  virtual bool publish(const CameraHealthEvent& event) = 0;
  virtual bool publishPrivacy(const CameraPrivacyEvent& event) = 0;
};
