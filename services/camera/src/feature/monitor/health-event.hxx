#pragma once

#include <camera/camera-privacy-state.hxx>
#include <shared/vocabulary/health-thresholds.hxx>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct HealthMetrics
{
  double brightness{0.0};
  double blur{0.0};
  double sceneDiff{0.0};
  bool paused{false};
  bool homeDrift{false};
  double saturated{0.0};
  double edgeRatio{1.0};
  double sharpness{1.0};
  double blocksLost{0.0};
  double shiftPx{0.0};
  double entropy{8.0};
  double chroma{0.0};
  bool infrared{false};
  bool sudden{false};
  bool held{false};
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
  Whiteout,
  Blinded,
  PartialCover,
  DarkSudden,
};

struct CameraHealthEvent
{
  int64_t cameraId{0};
  std::string cameraName;
  CameraHealthState status{CameraHealthState::Ok};
  HealthMetrics metrics;
  int64_t detectedAtMs{0};
  std::string link{"wifi"};
  int64_t onsetAtMs{0};
};

struct TamperOnset
{
  int64_t cameraId{0};
  std::string cameraName;
  CameraHealthState status{CameraHealthState::Ok};
  int64_t atMs{0};
  std::vector<uint8_t> lastGoodJpeg;
  int64_t lastGoodAtMs{0};
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
bool tampering(CameraHealthState status, bool sudden);
}

class IHealthEventSink
{
public:
  virtual ~IHealthEventSink() = default;
  virtual bool publish(const CameraHealthEvent& event) = 0;
  virtual bool publishPrivacy(const CameraPrivacyEvent& event) = 0;
};
