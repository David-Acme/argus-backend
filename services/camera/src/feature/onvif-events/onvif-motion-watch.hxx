#pragma once

#include <shared/services/camera-motion/camera-motion-board.hxx>
#include <shared/services/onvif/onvif-client.hxx>
#include <shared/services/onvif/onvif-transport.hxx>
#include <shared/services/onvif/onvif-types.hxx>

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <functional>
#include <memory>
#include <optional>
#include <trantor/net/EventLoop.h>

struct OnvifMotionWatchParts
{
  int64_t cameraId{0};
  OnvifEndpoint endpoint;
  std::shared_ptr<IOnvifAsyncTransport> transport;
  CameraMotionBoard* board{nullptr};
  trantor::EventLoop* loop{nullptr};
  std::function<int64_t()> clockMs;
  int64_t firstBackoffMs{5000};
  int64_t maxBackoffMs{60000};
  int64_t minPullGapMs{200};
};

class OnvifMotionWatch final : public std::enable_shared_from_this<OnvifMotionWatch>
{
public:
  static constexpr int64_t kFirstBackoffMs = 5000;
  static constexpr int64_t kMaxBackoffMs = 60000;
  static constexpr int64_t kDefaultLifetimeSeconds = 60;
  static constexpr int64_t kMinPullGapMs = 200;

  explicit OnvifMotionWatch(OnvifMotionWatchParts parts);

  void start();
  void stop();
  [[nodiscard]] bool finished() const;
  [[nodiscard]] bool parked() const;
  [[nodiscard]] const OnvifEndpoint& endpoint() const;

  [[nodiscard]] static int64_t nextBackoffMs(int64_t currentMs, int64_t maxMs);
  [[nodiscard]] static int64_t renewAfterMs(int64_t lifetimeSeconds);

private:
  class Pause;

  drogon::Task<void> run();
  drogon::Task<bool> follow();
  drogon::Task<void> release();
  void wake();
  void report(bool moving);
  void failed(const std::string& step, const std::string& error);

  OnvifMotionWatchParts parts_;
  OnvifClient client_;
  std::optional<OnvifSubscription> subscription_;
  std::atomic<bool> stopping_{false};
  std::atomic<bool> finished_{false};
  std::atomic<bool> parked_{false};
  bool failing_{false};
  std::coroutine_handle<> paused_;
  trantor::TimerId pauseTimer_{0};
};
