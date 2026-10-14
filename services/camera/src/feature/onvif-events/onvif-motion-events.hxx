#pragma once

#include <feature/onvif-events/onvif-motion-watch.hxx>
#include <shared/repositories/camera/camera-repository.hxx>
#include <shared/schemas/camera/camera-schema.hxx>
#include <shared/services/camera-motion/camera-motion-board.hxx>
#include <shared/services/onvif/onvif-types.hxx>

#include <atomic>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <functional>
#include <memory>
#include <mutex>
#include <trantor/net/EventLoopThread.h>
#include <unordered_map>
#include <vector>

struct OnvifEventsTarget
{
  int64_t cameraId{0};
  OnvifEndpoint endpoint;
};

struct OnvifMotionEventsParts
{
  CameraMotionBoard* board{nullptr};
  std::function<bool()> active;
};

class OnvifMotionEvents
{
public:
  static constexpr double kReconcileSeconds = 5.0;
  static constexpr int64_t kStopBudgetMs = 2000;

  explicit OnvifMotionEvents(OnvifMotionEventsParts parts);

  void start();
  void requestStop();
  [[nodiscard]] bool drained() const;

  [[nodiscard]] static std::vector<OnvifEventsTarget> targetsOf(const std::vector<CameraSchema>& cameras);

private:
  drogon::Task<void> reconcile();
  void apply(const std::vector<OnvifEventsTarget>& targets);
  void track(const std::shared_ptr<OnvifMotionWatch>& watch);
  [[nodiscard]] trantor::EventLoop* loop();

  OnvifMotionEventsParts parts_;
  CameraRepository repository_;
  std::unordered_map<int64_t, std::shared_ptr<OnvifMotionWatch>> watches_;
  std::unordered_map<int64_t, std::shared_ptr<OnvifMotionWatch>> retiring_;
  bool reconciling_{false};
  mutable std::mutex allMutex_;
  std::vector<std::shared_ptr<OnvifMotionWatch>> all_;
  std::mutex lifecycleMutex_;
  bool started_{false};
  std::atomic<bool> stopRequested_{false};
  std::atomic<int64_t> stopAtMs_{0};
  mutable std::atomic<bool> abandonLogged_{false};
  trantor::EventLoopThread thread_;
};
