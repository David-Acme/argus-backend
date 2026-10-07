#pragma once

#include <drogon/drogon.h>
#include <test-support/app-runner.hxx>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>

using test_support::AppRunner;

inline bool waitForBoot(std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (drogon::app().isRunning() && drogon::app().getLoop()->isRunning())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return drogon::app().isRunning() && drogon::app().getLoop()->isRunning();
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
