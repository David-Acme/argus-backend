#pragma once

#include <feature/camera-control/services/aim-settings.hxx>
#include <feature/camera-control/services/camera-control-feature-service.hxx>
#include <shared/repositories/camera/camera-repository.hxx>

#include <atomic>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <functional>
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

struct PatrolDependencies
{
  std::function<bool()> awayOrArmed{};
  std::function<bool()> active{};
};

struct PatrolDueInput
{
  bool enabled{false};
  bool awayOrArmed{false};
  bool cameraPatrols{false};
  bool leaseFree{false};
  int64_t lastTourMs{0};
  int64_t nowMs{0};
  int64_t everyMs{600000};
};

namespace patrol
{
[[nodiscard]] bool due(const PatrolDueInput& input);
}

class PatrolService
{
public:
  explicit PatrolService(PatrolDependencies dependencies);

  void start();
  void requestStop();
  [[nodiscard]] bool drained() const;

private:
  drogon::Task<void> tick();
  drogon::Task<void> tour(int64_t cameraId, AimSettings settings);

  PatrolDependencies dependencies_;
  CameraRepository repository_;
  CameraControlFeatureService control_;
  std::atomic<bool> running_{false};
  std::atomic<int64_t> inFlight_{0};
  std::mutex toursMutex_;
  std::unordered_map<int64_t, int64_t> lastTourMs_;
};
