#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/synthesis/infra/pocket/pocket-load.hxx>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <functional>
#include <string>
#include <thread>
#include <vector>

namespace
{

constexpr auto kComponentPause = std::chrono::milliseconds(50);
constexpr auto kShutdownBudget = std::chrono::seconds(10);

struct SlowComponentsInput
{
  std::size_t count{0};
  std::atomic<std::size_t>& loaded;
  std::function<void(std::size_t)> afterEach{};
};

std::vector<PocketLoadStep> slowComponents(const SlowComponentsInput& input)
{
  std::vector<PocketLoadStep> steps;
  steps.reserve(input.count);
  for (std::size_t index = 0; index < input.count; ++index) {
    steps.push_back({.name = "component " + std::to_string(index),
                     .load = [index, &loaded = input.loaded, afterEach = input.afterEach] {
                       std::this_thread::sleep_for(kComponentPause);
                       loaded.fetch_add(1);
                       if (afterEach)
                         afterEach(index);
                     }});
  }
  return steps;
}

}

TEST_CASE("a load without a stop request runs every component in order")
{
  std::atomic<std::size_t> loaded{0};
  const auto steps = slowComponents({.count = 3, .loaded = loaded, .afterEach = {}});
  CHECK_NOTHROW(runPocketLoad(steps, [] { return false; }));
  CHECK(loaded.load() == 3);
}

TEST_CASE("a stop requested between components ends the load before the next one")
{
  std::atomic<bool> stopping{false};
  std::atomic<std::size_t> loaded{0};
  const auto steps = slowComponents({.count = 10, .loaded = loaded, .afterEach = [&stopping](std::size_t index) {
    if (index == 1)
      stopping.store(true);
  }});
  CHECK_THROWS_AS(runPocketLoad(steps, [&stopping] { return stopping.load(); }), PocketLoadStopped);
  CHECK(loaded.load() == 2);
}

TEST_CASE("a drain started during the load returns well inside the shutdown budget")
{
  std::atomic<bool> stopping{false};
  std::atomic<std::size_t> loaded{0};
  const auto steps = slowComponents({.count = 100, .loaded = loaded, .afterEach = {}});
  std::atomic<bool> stopped{false};
  const auto started = std::chrono::steady_clock::now();
  std::jthread load([&] {
    try {
      runPocketLoad(steps, [&stopping] { return stopping.load(); });
    }
    catch (const PocketLoadStopped&) {
      stopped.store(true);
    }
  });
  while (loaded.load() == 0)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  stopping.store(true);
  load.join();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  CHECK(stopped.load());
  CHECK(loaded.load() < 100);
  CHECK(elapsed < kShutdownBudget);
}
