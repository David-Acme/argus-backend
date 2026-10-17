#pragma once

#include <camera/camera-privacy-state.hxx>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <shared/schemas/camera/camera-schema.hxx>
#include <vector>

struct PrivacyRefreshInput
{
  CameraSchema camera;
  Json::Value driverStatus;
  bool readable{false};
};

struct PrivacyTransition
{
  CameraPrivacyState before{CameraPrivacyState::Unknown};
  CameraPrivacyState after{CameraPrivacyState::Unknown};
  bool changed{false};
};

class PrivacyStateService
{
public:
  static PrivacyStateService& instance();

  void seed(const std::vector<CameraSchema>& cameras);
  [[nodiscard]] CameraPrivacyState stateOf(int64_t cameraId) const;
  [[nodiscard]] bool masked(int64_t cameraId) const;
  [[nodiscard]] bool maskedOf(const CameraSchema& camera) const;
  [[nodiscard]] static bool privacyCapable(const CameraSchema& camera);
  [[nodiscard]] drogon::Task<PrivacyTransition>
  refresh(PrivacyRefreshInput input);
  void forget(int64_t cameraId);

private:
  PrivacyStateService() = default;
};
