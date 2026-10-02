#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>
#include <runtime/wake-signal.hxx>

#include <chrono>

TEST_CASE("a notify before the wait is not lost")
{
  WakeSignal signal;
  signal.notify();

  const auto start = std::chrono::steady_clock::now();
  signal.waitFor(std::chrono::seconds(5));
  CHECK(std::chrono::steady_clock::now() - start < std::chrono::seconds(1));
}

TEST_CASE("a wait consumes the notify and the next one sleeps its timeout")
{
  WakeSignal signal;
  signal.notify();
  signal.waitFor(std::chrono::seconds(5));

  const auto start = std::chrono::steady_clock::now();
  signal.waitFor(std::chrono::milliseconds(60));
  CHECK(std::chrono::steady_clock::now() - start >=
        std::chrono::milliseconds(50));
}
