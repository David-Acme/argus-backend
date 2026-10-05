#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include "app-runner.hxx"

#include <drogon/drogon.h>
#include <runtime/shutdown-signal.hxx>

#include <chrono>
#include <sys/wait.h>
#include <unistd.h>

namespace
{

struct StuckDrain
{
  int stops{0};

  void requestStop() { ++stops; }

  [[nodiscard]] bool drained() const { return false; }
};

}

TEST_CASE("the default deadline leaves room inside the compose stop grace period")
{
  CHECK(shutdown_signal::kDefaultDeadline == std::chrono::seconds(15));
  CHECK(shutdown_signal::deadline() == shutdown_signal::kDefaultDeadline);
}

TEST_CASE("a second termination signal forces the process out at once")
{
  const pid_t child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    shutdown_signal::onSignal();
    shutdown_signal::onSignal();
    ::_exit(0);
  }
  int status = 0;
  REQUIRE(::waitpid(child, &status, 0) == child);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == shutdown_signal::kForcedExitCode);
}

TEST_CASE("a single termination signal only records the request")
{
  const pid_t child = ::fork();
  REQUIRE(child >= 0);
  if (child == 0) {
    shutdown_signal::onSignal();
    ::_exit(0);
  }
  int status = 0;
  REQUIRE(::waitpid(child, &status, 0) == child);
  REQUIRE(WIFEXITED(status));
  CHECK(WEXITSTATUS(status) == 0);
}

TEST_CASE("a drain that never finishes is abandoned at the configured deadline")
{
  drogon::app().setLogLevel(trantor::Logger::kFatal);
  shutdown_signal::setDeadline(std::chrono::milliseconds(-5));
  CHECK(shutdown_signal::deadline() == std::chrono::milliseconds(0));
  shutdown_signal::setDeadline(std::chrono::milliseconds(200));
  CHECK(shutdown_signal::deadline() == std::chrono::milliseconds(200));

  AppRunner runner;
  REQUIRE(waitForBoot(std::chrono::seconds(30)));

  StuckDrain stuck;
  shutdown_signal::onStop(shutdown_signal::drainOf(stuck, "stuck"));
  shutdown_signal::requestStop();
  CHECK(stuck.stops == 1);
  CHECK(waitUntil([] { return !drogon::app().isRunning(); },
                  std::chrono::seconds(5)));
}
