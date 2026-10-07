#pragma once

#include <drogon/drogon.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <thread>

namespace test_support
{
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
    waitForLoop();
  }

  ~AppRunner()
  {
    if (!runner_.joinable())
      return;
    waitForLoop();
    if (finished_->load(std::memory_order_acquire)) {
      runner_.join();
      return;
    }
    if (drogon::app().getLoop()->isRunning()) {
      drogon::app().quit();
      runner_.join();
      return;
    }
    runner_.detach();
  }

  AppRunner(const AppRunner&) = delete;
  AppRunner& operator=(const AppRunner&) = delete;

private:
  static constexpr int kPolls = 3000;
  static constexpr std::chrono::milliseconds kPoll{10};

  void waitForLoop() const
  {
    for (int i = 0; i < kPolls && !finished_->load(std::memory_order_acquire) &&
                    !drogon::app().getLoop()->isRunning();
         ++i)
      std::this_thread::sleep_for(kPoll);
  }

  std::shared_ptr<std::atomic<bool>> finished_;
  std::thread runner_;
};
}
