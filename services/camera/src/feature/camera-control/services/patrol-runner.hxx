#pragma once

#include <feature/camera-control/repositories/camera-patrol/camera-patrol-repository.hxx>
#include <feature/camera-control/services/camera-control-feature-service.hxx>
#include <feature/camera-control/services/patrol-plan.hxx>

#include <atomic>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <functional>
#include <mutex>
#include <optional>
#include <trantor/net/EventLoop.h>
#include <unordered_map>
#include <vector>

struct PatrolRunnerConfig
{
  int64_t settleMs{4000};
  double tickSeconds{2.0};
  std::function<bool()> active{};
  std::function<bool()> awayOrArmed{};
  std::function<bool(int64_t)> busy{};
};

class PatrolRunner
{
public:
  explicit PatrolRunner(PatrolRunnerConfig config);
  ~PatrolRunner() = default;

  PatrolRunner(const PatrolRunner&) = delete;
  PatrolRunner& operator=(const PatrolRunner&) = delete;

  void start();
  void requestStop();
  [[nodiscard]] bool drained() const;

  drogon::Task<void> tick();

private:
  struct PatrolTarget
  {
    int64_t cameraId{0};
    std::optional<CameraPatrolSchema> patrol;
    bool online{false};
  };

  struct PatrolClock
  {
    int64_t nowMs{0};
    int localHour{0};
    bool moduleActive{false};
    bool allowed{false};
    int64_t everyMs{0};
  };

  drogon::Task<void> follow(PatrolTarget target, PatrolClock moment);
  [[nodiscard]] PatrolProgress progressOf(int64_t cameraId);
  void keep(int64_t cameraId, const PatrolProgress& progress);
  [[nodiscard]] std::vector<int64_t> activeCameras();
  void prune(const std::vector<int64_t>& seen);

  PatrolRunnerConfig config_;
  CameraPatrolRepository repository_;
  CameraControlFeatureService service_;
  std::mutex mutex_;
  std::unordered_map<int64_t, PatrolProgress> progress_;
  std::atomic<bool> running_{false};
  std::atomic<bool> ticking_{false};
  std::atomic<int64_t> inFlight_{0};
  std::optional<trantor::TimerId> timer_;
};
