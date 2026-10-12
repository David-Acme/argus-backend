#pragma once

#include <atomic>
#include <cstdint>
#include <ctime>
#include <drogon/utils/coroutine.h>
#include <functional>
#include <optional>
#include <trantor/net/EventLoop.h>

class ResourceChangeOutbox;

class PostureWatch
{
public:
  struct Clock
  {
    std::function<int64_t()> now;
    std::function<std::tm()> local;
  };

  struct Dependencies
  {
    const ResourceChangeOutbox* changes{nullptr};
  };

  struct Config
  {
    double intervalSeconds{30};
    Clock clock{};
  };

  PostureWatch(Dependencies dependencies, Config config);

  void start();
  void requestStop();
  [[nodiscard]] bool drained() const;
  [[nodiscard]] drogon::Task<void> evaluate() const;

private:
  void schedule();

  Dependencies dependencies_;
  Config config_;
  std::optional<trantor::TimerId> timer_;
  std::atomic<int> pending_{0};
};
