#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/drogon.h>
#include <runtime/shutdown-signal.hxx>

#include <atomic>
#include <chrono>
#include <functional>
#include <string>
#include <thread>

namespace
{

class AppRunner
{
public:
  AppRunner() : runner_([] { drogon::app().run(); }) {}

  ~AppRunner()
  {
    if (!runner_.joinable())
      return;
    for (int i = 0; i < 3000 && !drogon::app().getLoop()->isRunning(); ++i)
      std::this_thread::sleep_for(std::chrono::milliseconds(10));
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
  std::thread runner_;
};

bool waitForBoot(std::chrono::milliseconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (drogon::app().isRunning())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(10));
  }
  return drogon::app().isRunning();
}

bool waitUntil(const std::function<bool()>& ready,
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
