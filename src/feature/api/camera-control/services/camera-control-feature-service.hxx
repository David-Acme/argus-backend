#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/api/camera-control/dtos/camera-preset-dto.hxx>
#include <feature/api/camera-control/dtos/camera-ptz-dto.hxx>
#include <feature/api/camera-control/dtos/camera-settings-dto.hxx>
#include <feature/api/camera-control/dtos/camera-talk-dto.hxx>
#include <json/value.h>
#include <optional>
#include <shared/repositories/camera/camera-repository.hxx>
#include <shared/services/camera-driver/camera-driver.hxx>

/** Outcome of a device call: nullopt means the camera row does not exist. */
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
  /** Synthesizes the text and plays it on the camera speaker. */
  drogon::Task<CameraControlResult> speak(int64_t cameraId,
                                          const CameraTalkDto& body) const;
  drogon::Task<CameraControlResult> capabilities(int64_t cameraId) const;

private:
  /** Every device call is blocking, so it runs off the event loop. */
  drogon::Task<CameraControlResult>
  onDevice(int64_t cameraId,
           const std::function<DriverResult(ICameraDriver&)>& work) const;

  CameraRepository repository_;
};
