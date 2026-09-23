#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "app-runner.hxx"

#include <drogon/drogon.h>
#include <runtime/shutdown-signal.hxx>

#include <atomic>
#include <chrono>
#include <thread>

namespace
{

struct FakeDrain
{
  std::atomic<int> stops{0};
  std::atomic<bool> finished{false};

  void requestStop() { stops.fetch_add(1, std::memory_order_acq_rel); }

  [[nodiscard]] bool drained() const
  {
    return finished.load(std::memory_order_acquire);
  }
};

void settle(std::chrono::milliseconds window)
{
  std::this_thread::sleep_for(window);
}

}

TEST_CASE("the quit hook runs once, on the loop, after the drains reported drained")
{
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));

  FakeDrain pending;
  std::atomic<int> runs{0};
  std::atomic<bool> onLoop{false};
  std::atomic<bool> stillRunning{false};

  shutdown_signal::onStop(shutdown_signal::drainOf(pending, "pending"));
  shutdown_signal::onQuit([&] {
    runs.fetch_add(1, std::memory_order_acq_rel);
    onLoop.store(drogon::app().getLoop()->isInLoopThread(),
                 std::memory_order_release);
    stillRunning.store(drogon::app().isRunning(), std::memory_order_release);
  });

  shutdown_signal::requestStop();
  CHECK(pending.stops.load(std::memory_order_acquire) == 1);
  settle(std::chrono::milliseconds(300));
  CHECK(runs.load(std::memory_order_acquire) == 0);
  CHECK(drogon::app().isRunning());

  pending.finished.store(true, std::memory_order_release);
  REQUIRE(waitUntil([] { return !drogon::app().isRunning(); },
                    std::chrono::seconds(5)));

  CHECK(runs.load(std::memory_order_acquire) == 1);
  CHECK(onLoop.load(std::memory_order_acquire));
  CHECK(stillRunning.load(std::memory_order_acquire));
}
