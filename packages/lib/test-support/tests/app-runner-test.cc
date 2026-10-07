#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <test-support/app-runner.hxx>

#include <chrono>
#include <optional>
#include <thread>

TEST_CASE("a runner whose app the test already quit is released at once")
{
  std::optional<test_support::AppRunner> runner;
  runner.emplace();
  REQUIRE(drogon::app().getLoop()->isRunning());

  drogon::app().quit();
  const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
  while (drogon::app().getLoop()->isRunning() && std::chrono::steady_clock::now() < deadline)
    std::this_thread::sleep_for(std::chrono::milliseconds(1));
  REQUIRE_FALSE(drogon::app().getLoop()->isRunning());

  const auto began = std::chrono::steady_clock::now();
  runner.reset();
  CHECK(std::chrono::steady_clock::now() - began < std::chrono::seconds(5));
}
