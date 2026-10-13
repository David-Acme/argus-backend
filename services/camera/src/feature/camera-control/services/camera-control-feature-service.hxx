#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/camera-control/dtos/camera-preset-dto.hxx>
#include <feature/camera-control/dtos/camera-ptz-dto.hxx>
#include <feature/camera-control/dtos/camera-sd-dto.hxx>
#include <feature/camera-control/dtos/camera-settings-dto.hxx>
#include <feature/camera-control/dtos/camera-snapshot-dto.hxx>
#include <feature/camera-control/dtos/camera-talk-dto.hxx>
#include <json/value.h>
#include <optional>
#include <shared/repositories/camera/camera-repository.hxx>
#include <shared/repositories/zone/zone-repository.hxx>
#include <shared/services/camera-driver/camera-driver.hxx>
#include <shared/services/stream/go2rtc-frame-source.hxx>
#include <tts/tts-remote.hxx>
#include <vector>

using CameraControlResult = std::optional<DriverResult>;

class CameraControlFeatureService
{
public:
  drogon::Task<CameraControlResult> status(int64_t cameraId) const;
  drogon::Task<CameraControlResult> presets(int64_t cameraId) const;
  drogon::Task<CameraControlResult> move(int64_t cameraId,
                                         const CameraPtzDto& body) const;
  drogon::Task<CameraControlResult> preset(int64_t cameraId,
                                          const CameraPresetDto& body) const;
  drogon::Task<CameraControlResult> settings(int64_t cameraId,
                                            const CameraSettingsDto& body) const;
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

  drogon::Task<std::optional<std::vector<uint8_t>>> masked(int64_t cameraId,
                                                           std::vector<uint8_t> jpeg) const;

  CameraRepository cameraRepository_;
  ZoneRepository zoneRepository_;
  TtsClient tts_;
  mutable Go2rtcFrameSource frames_;
};
