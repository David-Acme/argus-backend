#pragma once

#include <feature/camera-control/services/aim-settings.hxx>
#include <feature/camera-control/services/camera-control-feature-service.hxx>
#include <feature/watch/services/watch-aim-plan.hxx>
#include <feature/watch/services/watch-camera-board.hxx>

#include <argus/camera/v1/watch.pb.h>
#include <config/camera-config.hxx>
#include <json/value.h>
#include <shared/services/stream/person-sightings.hxx>

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <vector>

struct AimGeometry
{
  double horizontalFovDegrees{100.0};
  double verticalFovDegrees{56.0};
  double deadZone{0.05};
};

struct AimStep
{
  int64_t pan{0};
  int64_t tilt{0};
  bool centered{false};
};

struct AimTarget
{
  int64_t cameraId{0};
  std::string conversationId;
};

struct SightingChoice
{
  std::span<const PersonSighting> sightings;
  int64_t notBeforeMs{0};
};

[[nodiscard]] AimStep aimStepFor(const PersonSighting& sighting, const AimGeometry& geometry);

[[nodiscard]] std::optional<PersonSighting> bestSighting(const SightingChoice& choice);

[[nodiscard]] std::vector<std::string> presetIdsOf(const Json::Value& data);

[[nodiscard]] AimGeometry aimGeometryOf(const CameraWatchConfig& config);

struct WatchAimerInput
{
  std::shared_ptr<WatchCameraBoard> board;
  std::function<argus::camera::v1::WatchRefusal(int64_t)> gate;
  CameraWatchConfig config;
  std::function<void(std::chrono::milliseconds, std::function<void()>)> schedule;
  std::function<std::vector<RadarFix>(int64_t)> radar;
};

class WatchAimer
{
public:
  explicit WatchAimer(WatchAimerInput input);

  [[nodiscard]] argus::camera::v1::AimResponse find(const AimTarget& target);
  [[nodiscard]] argus::camera::v1::AimResponse release(const AimTarget& target);
  [[nodiscard]] bool holds(int64_t cameraId);

private:
  struct Holding
  {
    std::string conversationId;
    uint64_t generation{0};
  };

  struct CenterRequest
  {
    ICameraDriver& driver;
    int64_t cameraId{0};
    const PersonSighting& sighting;
    const AimSettings& settings;
    std::string detail;
  };

  struct TurnRequest
  {
    ICameraDriver& driver;
    int64_t cameraId{0};
    AimAngles delta;
  };

  struct HoldCheck
  {
    int64_t cameraId{0};
    AimAngles delta;
    const AimSettings& settings;
  };

  struct PresetRequest
  {
    ICameraDriver& driver;
    int64_t cameraId{0};
    const std::string& presetId;
  };

  struct RadarRequest
  {
    ICameraDriver& driver;
    int64_t cameraId{0};
    const AimSettings& settings;
  };

  [[nodiscard]] std::optional<PersonSighting> lookFor(int64_t cameraId, int64_t notBeforeMs) const;
  [[nodiscard]] std::optional<PersonSighting> waitFor(int64_t cameraId, int64_t fromMs) const;
  [[nodiscard]] AimSettings settingsOf(int64_t cameraId) const;
  [[nodiscard]] static bool wearAllows(int64_t cameraId);
  [[nodiscard]] static AimHold holdOf(const HoldCheck& check);
  [[nodiscard]] argus::camera::v1::AimResponse centerOn(const CenterRequest& request) const;
  [[nodiscard]] std::optional<argus::camera::v1::AimResponse> radarStep(const RadarRequest& request) const;
  [[nodiscard]] bool turn(const TurnRequest& request) const;
  [[nodiscard]] bool goPreset(const PresetRequest& request) const;
  static void markAway(int64_t cameraId);
  void hold(const AimTarget& target);
  void returnWhenIdle(int64_t cameraId, uint64_t generation);
  drogon::Task<void> returnHome(int64_t cameraId) const;

  WatchAimerInput input_;
  AimGeometry geometry_;
  CameraControlFeatureService control_;
  std::mutex mutex_;
  std::unordered_map<int64_t, Holding> holding_;
  uint64_t nextGeneration_{1};
};
