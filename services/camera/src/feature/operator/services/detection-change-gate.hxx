#pragma once

#include <atomic>
#include <cstdint>
#include <ctime>
#include <drogon/utils/coroutine.h>
#include <functional>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <trantor/net/EventLoop.h>

class DetectionChangeGate
{
public:
  struct Config
  {
    double publishSeconds{15};
    std::function<int64_t()> clock;
  };

  explicit DetectionChangeGate(Config config);

  [[nodiscard]] drogon::Task<void> detected(int64_t cameraId);
  [[nodiscard]] drogon::Task<void> flush();

  void start();
  void requestStop();
  [[nodiscard]] bool drained() const;

private:
  struct DueInput
  {
    int64_t cameraId{0};
    int64_t at{0};
  };

  [[nodiscard]] bool dueLocked(const DueInput& input) const;
  [[nodiscard]] int64_t now() const;
  void schedule();

  Config config_;
  mutable std::mutex mutex_;
  std::map<int64_t, int64_t> publishedAt_;
  std::set<int64_t> dirty_;
  std::optional<trantor::TimerId> timer_;
  std::atomic<bool> stopped_{false};
  std::atomic<int> pending_{0};
};
