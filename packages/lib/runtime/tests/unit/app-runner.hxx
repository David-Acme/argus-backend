#pragma once

#include <drogon/drogon.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

class AppRunner
{
public:
  AppRunner()
      : finished_(std::make_shared<std::atomic<bool>>(false)),
        runner_([flag = finished_] {
          drogon::app().run();
          flag->store(true, std::memory_order_release);
        })
  {
  }

  ~AppRunner()
  {
    if (!runner_.joinable())
      return;
    for (int i = 0; i < 3000 && !finished_->load(std::memory_order_acquire) &&
                    !drogon::app().isRunning(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
    if (drogon::app().isRunning()) {
      drogon::app().quit();
      for (int i = 0; i < 3000 && !finished_->load(std::memory_order_acquire);
           ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (finished_->load(std::memory_order_acquire))
      runner_.join();
    else
      runner_.detach();
  }

  AppRunner(const AppRunner&) = delete;
  AppRunner& operator=(const AppRunner&) = delete;

private:
  std::shared_ptr<std::atomic<bool>> finished_;
  std::thread runner_;
};

inline bool waitForBoot(std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (drogon::app().isRunning())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return drogon::app().isRunning();
}

inline bool waitUntil(const std::function<bool()>& ready,
                      std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (ready())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return ready();
}
