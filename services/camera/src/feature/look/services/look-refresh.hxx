#pragma once

#include <feature/camera-control/services/aim-settings.hxx>
#include <shared/repositories/camera/camera-repository.hxx>

#include <atomic>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <mutex>
#include <optional>
#include <unordered_map>

class LookableCameras
{
public:
  static LookableCameras& instance();

  void replace(std::unordered_map<int64_t, AimSettings> settings);
  [[nodiscard]] std::optional<AimSettings> of(int64_t cameraId);
  [[nodiscard]] bool lookable(int64_t cameraId);

private:
  std::mutex mutex_;
  std::unordered_map<int64_t, AimSettings> settings_;
};

class LookRefresh
{
public:
  void start();
  void requestStop();
  [[nodiscard]] bool drained() const;

private:
  drogon::Task<void> tick();

  CameraRepository repository_;
  std::atomic<bool> running_{false};
  std::atomic<int64_t> inFlight_{0};
};
