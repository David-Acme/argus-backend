#pragma once

#include <auth/user-role.hxx>
#include <camera/camera-privacy-state.hxx>
#include <config/camera-config.hxx>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/camera-control/dtos/camera-preset-dto.hxx>
#include <feature/camera-control/dtos/camera-ptz-dto.hxx>
#include <feature/camera-control/dtos/camera-sd-dto.hxx>
#include <feature/camera-control/dtos/camera-settings-dto.hxx>
#include <feature/camera-control/dtos/camera-snapshot-dto.hxx>
#include <feature/camera-control/dtos/camera-talk-dto.hxx>
#include <feature/camera-control/services/ptz-lease.hxx>
#include <feature/camera-control/services/still-flights.hxx>
#include <json/value.h>
#include <optional>
#include <string>
#include <shared/repositories/camera/camera-repository.hxx>
#include <shared/repositories/zone/zone-repository.hxx>
#include <shared/services/camera-driver/camera-driver.hxx>
#include <shared/services/stream/go2rtc-frame-source.hxx>
#include <tts/tts-remote.hxx>
#include <vector>

using CameraControlResult = std::optional<DriverResult>;

struct CameraSettingsInput
{
  int64_t cameraId{0};
  CameraSettingsDto body{};
  int64_t actorUserId{0};
  std::string actorIp{};
};

struct CameraMoveInput
{
  int64_t cameraId{0};
  CameraPtzDto body{};
  int64_t actorUserId{0};
  UserRole actorRole{UserRole::Unknown};
};

struct CameraPresetInput
{
  int64_t cameraId{0};
  CameraPresetDto body{};
  int64_t actorUserId{0};
  UserRole actorRole{UserRole::Unknown};
};

struct CameraReturnHomeInput
{
  int64_t cameraId{0};
  std::string reason;
};

struct CameraPatrolMoveInput
{
  int64_t cameraId{0};
  std::string presetId;
};

struct CameraPatrolEndInput
{
  int64_t cameraId{0};
  bool returnHome{false};
};

struct CameraPatrolEvent
{
  int64_t cameraId{0};
  std::string phase;
  std::string reason;
};

class CameraControlFeatureService
{
public:
  drogon::Task<CameraControlResult> status(int64_t cameraId) const;
  drogon::Task<CameraControlResult> presets(int64_t cameraId) const;
  drogon::Task<CameraControlResult> move(const CameraMoveInput& input) const;
  drogon::Task<CameraControlResult> preset(const CameraPresetInput& input) const;
  drogon::Task<CameraControlResult> returnHome(const CameraReturnHomeInput& input) const;
  drogon::Task<CameraControlResult> patrolMove(const CameraPatrolMoveInput& input) const;
  drogon::Task<void> endPatrol(const CameraPatrolEndInput& input) const;
  drogon::Task<void> recordPatrol(const CameraPatrolEvent& event) const;
  drogon::Task<CameraControlResult> calibrate(int64_t cameraId) const;
  static void configurePtz(const CameraPtzConfig& config);
  drogon::Task<CameraControlResult> settings(const CameraSettingsInput& input) const;
  drogon::Task<CameraControlResult> speak(int64_t cameraId,
                                          const CameraTalkDto& body) const;
  drogon::Task<CameraControlResult> capabilities(int64_t cameraId) const;
  drogon::Task<CameraControlResult> snapshot(int64_t cameraId, CameraStream stream) const;
  drogon::Task<CameraControlResult> sdDays(int64_t cameraId, const CameraSdDaysDto& query) const;
  drogon::Task<CameraControlResult> sdDay(int64_t cameraId, const CameraSdDayDto& query) const;

private:
  drogon::Task<CameraControlResult>
  onDevice(int64_t cameraId,
           const std::function<DriverResult(ICameraDriver&)>& work) const;

  drogon::Task<void> announceStatus(int64_t cameraId) const;

  drogon::Task<CameraControlResult> setHome(const CameraPresetInput& input) const;

  struct ActorCheck
  {
    int64_t userId{0};
    UserRole role{UserRole::Unknown};
    bool changesPresets{false};
  };

  static drogon::Task<void> admitActor(const ActorCheck& actor);

  struct AutomaticMove
  {
    int64_t cameraId{0};
    std::string reason;
    std::string target;
    int64_t durationMs{0};
  };

  drogon::Task<void> recordAutomaticMove(const AutomaticMove& move) const;

  drogon::Task<void> announcePrivacy(int64_t cameraId) const;

  drogon::Task<void> recordPrivacyAction(const CameraSettingsInput& input,
                                         CameraPrivacyState before,
                                         CameraPrivacyState after) const;

  drogon::Task<std::optional<std::vector<uint8_t>>> masked(int64_t cameraId,
                                                           std::vector<uint8_t> jpeg) const;
  drogon::Task<DriverResult> freshStill(FrameGrabRequest request) const;
  drogon::Task<DriverResult> sharedStill(FrameGrabRequest request) const;

  CameraRepository cameraRepository_;
  ZoneRepository zoneRepository_;
  TtsClient tts_;
  mutable Go2rtcFrameSource frames_;
  mutable StillFlights mainStills_;
};
