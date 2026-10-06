#pragma once

#include <drogon/drogon.h>

#include <chrono>
#include <thread>

namespace test_support
{
class AppRunner
{
public:
  AppRunner() : runner_([] { drogon::app().run(); }) { waitForLoop(); }

  ~AppRunner()
  {
    if (!runner_.joinable())
      return;
    waitForLoop();
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

  static void waitForLoop()
  {
    for (int i = 0; i < kPolls && !drogon::app().getLoop()->isRunning(); ++i)
      std::this_thread::sleep_for(kPoll);
  }

  std::thread runner_;
};
}
