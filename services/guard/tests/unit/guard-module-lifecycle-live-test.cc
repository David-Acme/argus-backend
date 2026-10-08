#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "guard-fakes.hxx"

#include <doctest/doctest.h>

#include <auth/role-access.hxx>
#include <feature/guard/guard-service.hxx>
#include <nats/live-broker.hxx>
#include <nats/nats-bus.hxx>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using guard_test::GuardBoot;

namespace
{
GuardBoot& boot()
{
  static GuardBoot shared("guard-module-lifecycle-live-test");
  return shared;
}

std::string unique(const char* stem)
{
  return std::string(stem) + "-" + std::to_string(::getpid());
}

bool waitUntil(const std::function<bool()>& ready, std::chrono::seconds timeout)
{
  const auto deadline = std::chrono::steady_clock::now() + timeout;
  while (std::chrono::steady_clock::now() < deadline) {
    if (ready())
      return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return ready();
}
}

TEST_CASE("a disabled guard still ensures ARGUS_GUARD at boot" *
          doctest::skip(live_broker::skipped()))
{
  const auto broker = live_broker::url();
  REQUIRE_MESSAGE(!broker.empty(), live_broker::kMissingUrl);
  boot();

  NatsBus bus;
  NatsBus::Options options;
  options.url = broker;
  options.reconnectWaitMs = 200;
  options.maxReconnects = 5;
  REQUIRE(bus.connect(options));

  GuardService::Config config;
  config.enabled = false;
  GuardService service({.bus = &bus,
                        .identity = nullptr,
                        .notifications = nullptr,
                        .actions = nullptr,
                        .assessment = nullptr},
                       config);
  service.start();

  const auto status = bus.streamInfo("ARGUS_GUARD");
  REQUIRE(status.has_value());
  CHECK(status->subjects == std::vector<std::string>{"argus.guard.v1.>"});
  CHECK_FALSE(service.watching());
}

TEST_CASE("the guard watches while surveillance is active and stops when it is not" *
          doctest::skip(live_broker::skipped()))
{
  const auto broker = live_broker::url();
  REQUIRE_MESSAGE(!broker.empty(), live_broker::kMissingUrl);
  boot();

  NatsBus bus;
  NatsBus::Options options;
  options.url = broker;
  options.reconnectWaitMs = 200;
  options.maxReconnects = 5;
  REQUIRE(bus.connect(options));

  std::atomic<bool> active{false};
  const std::string cameraStream = unique("ARGUS_CAMERA_LIFECYCLE");
  const std::string cameraSubject =
      "argus.test.guard.lifecycle." + std::to_string(::getpid());
  GuardService::Config config;
  config.enabled = true;
  config.eventStream = cameraStream;
  config.eventSubject = cameraSubject;
  config.consumerDurable = unique("guard-lifecycle");
  config.heartbeatS = 3600;
  GuardService service({.bus = &bus,
                        .identity = nullptr,
                        .notifications = nullptr,
                        .actions = nullptr,
                        .assessment = nullptr,
                        .active = [&active] { return active.load(std::memory_order_acquire); }},
                       config);

  service.start();
  CHECK_FALSE(service.watching());

  active.store(true, std::memory_order_release);
  service.start();
  CHECK_FALSE(service.watching());

  active.store(false, std::memory_order_release);
  service.stop();
  REQUIRE(bus.ensureStream({.name = cameraStream,
                            .subjects = {cameraSubject},
                            .maxAgeNs = 3600LL * 1000000000,
                            .duplicatesNs = 0}));
  std::this_thread::sleep_for(std::chrono::seconds(6));
  CHECK_FALSE(service.watching());

  active.store(false, std::memory_order_release);
  service.start();
  std::this_thread::sleep_for(std::chrono::milliseconds(500));
  CHECK_FALSE(service.watching());

  active.store(true, std::memory_order_release);
  service.start();
  CHECK(waitUntil([&service] { return service.watching(); }, std::chrono::seconds(10)));
  CHECK(bus.streamInfo(config.guardStream).has_value());

  service.stop();
  CHECK_FALSE(service.watching());
  service.requestStop();
  bus.drain();
}

TEST_CASE("a surveillance module change starts and stops the watch" *
          doctest::skip(live_broker::skipped()))
{
  const auto broker = live_broker::url();
  REQUIRE_MESSAGE(!broker.empty(), live_broker::kMissingUrl);
  boot();

  NatsBus bus;
  NatsBus::Options options;
  options.url = broker;
  options.reconnectWaitMs = 200;
  options.maxReconnects = 5;
  REQUIRE(bus.connect(options));

  const std::string cameraStream = unique("ARGUS_CAMERA_CHANGE");
  const std::string cameraSubject =
      "argus.test.guard.change." + std::to_string(::getpid());
  GuardService::Config config;
  config.enabled = true;
  config.eventStream = cameraStream;
  config.eventSubject = cameraSubject;
  config.consumerDurable = unique("guard-change");
  config.heartbeatS = 3600;
  GuardService service({.bus = &bus,
                        .identity = nullptr,
                        .notifications = nullptr,
                        .actions = nullptr,
                        .assessment = nullptr},
                       config);
  REQUIRE(bus.ensureStream({.name = cameraStream,
                            .subjects = {cameraSubject},
                            .maxAgeNs = 3600LL * 1000000000,
                            .duplicatesNs = 0}));
  service.start();
  CHECK(waitUntil([&service] { return service.watching(); }, std::chrono::seconds(10)));

  service.applyModuleChange(
      {.id = std::string(role_access::kSurveillanceModule), .enabled = false});
  CHECK(waitUntil([&service] { return !service.watching(); }, std::chrono::seconds(10)));

  service.applyModuleChange(
      {.id = std::string(role_access::kSurveillanceModule), .enabled = true});
  CHECK(waitUntil([&service] { return service.watching(); }, std::chrono::seconds(10)));

  service.applyModuleChange({.id = "productivity", .enabled = false});
  std::this_thread::sleep_for(std::chrono::milliseconds(200));
  CHECK(service.watching());

  service.stop();
  service.requestStop();
  bus.drain();
}
