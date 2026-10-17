#pragma once

#include <camera/camera-privacy-state.hxx>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <json/value.h>
#include <mutex>
#include <shared/repositories/camera/camera-repository.hxx>
#include <shared/schemas/camera/camera-schema.hxx>
#include <unordered_map>
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
  struct Entry
  {
    CameraPrivacyState state{CameraPrivacyState::Unknown};
    CameraPrivacyState persisted{CameraPrivacyState::Unknown};
    bool writing{false};
  };

  PrivacyStateService() = default;

  [[nodiscard]] drogon::Task<bool> persistState(int64_t cameraId,
                                                CameraPrivacyState state);

  mutable std::mutex mutex_;
  std::unordered_map<int64_t, Entry> entries_;
  CameraRepository repository_;
};
