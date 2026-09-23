#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "app-runner.hxx"

#include <drogon/drogon.h>
#include <runtime/shutdown-signal.hxx>

#include <atomic>
#include <chrono>
#include <string>
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

TEST_CASE("the shutdown hook defers quit until every registered drain drained")
{
  drogon::app().setLogLevel(trantor::Logger::kWarn);
  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));

  FakeDrain first;
  FakeDrain second;
  shutdown_signal::onStop(shutdown_signal::drainOf(first, "first"));
  shutdown_signal::onStop(shutdown_signal::drainOf(second, "second"));

  CHECK_FALSE(shutdown_signal::drained());
  first.finished.store(true, std::memory_order_release);
  CHECK_FALSE(shutdown_signal::drained());
  second.finished.store(true, std::memory_order_release);
  CHECK(shutdown_signal::drained());

  shutdown_signal::requestStop();
  CHECK(first.stops.load(std::memory_order_acquire) == 1);
  CHECK(second.stops.load(std::memory_order_acquire) == 1);
  shutdown_signal::requestStop();
  CHECK(first.stops.load(std::memory_order_acquire) == 1);
  CHECK(second.stops.load(std::memory_order_acquire) == 1);

  FakeDrain late;
  shutdown_signal::onStop(shutdown_signal::drainOf(late, "late"));
  CHECK(late.stops.load(std::memory_order_acquire) == 1);
  CHECK_FALSE(late.drained());
  CHECK(shutdown_signal::drained());

  first.finished.store(false, std::memory_order_release);
  second.finished.store(false, std::memory_order_release);

  settle(std::chrono::milliseconds(300));
  CHECK(drogon::app().isRunning());

  first.finished.store(true, std::memory_order_release);
  settle(std::chrono::milliseconds(200));
  CHECK(drogon::app().isRunning());

  second.finished.store(true, std::memory_order_release);
  CHECK(waitUntil([] { return !drogon::app().isRunning(); },
                  std::chrono::seconds(5)));
}
