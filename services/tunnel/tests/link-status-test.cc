#include <doctest/doctest.h>

#include <link/protocol/client-hello.hxx>
#include <link/status/link-status.hxx>
#include <routes/remote-link.hxx>

#include <json/json.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
constexpr std::string_view kHomeId = "0123456789abcdef0123456789abcdef";
constexpr int64_t kStart = 1'790'000'000;

struct Harness
{
  int64_t now{kStart};
  tunnel::link::LinkSample sample;
  bool busUp{true};
  std::vector<std::string> sent;
  std::unique_ptr<tunnel::link::LinkStatusPublisher> publisher;

  Harness()
  {
    publisher = std::make_unique<tunnel::link::LinkStatusPublisher>(
        tunnel::link::LinkStatusPublisherDeps{.sample = [this] { return sample; },
                                              .publish =
                                                  [this](std::string_view payload) {
                                                    if (!busUp)
                                                      return false;
                                                    sent.emplace_back(payload);
                                                    return true;
                                                  },
                                              .now = [this] { return now; }},
        tunnel::link::LinkStatusPublisherOptions{
            .homeId = std::string(kHomeId), .domain = "argus-tunnel.lynk.page", .republish = std::chrono::seconds{30}});
  }

  void tickAt(int64_t at)
  {
    now = at;
    publisher->tick();
  }
};

routes::RemoteLinkState decoded(const std::string& payload)
{
  Json::Value json;
  Json::CharReaderBuilder builder;
  std::string errors;
  std::istringstream stream(payload);
  REQUIRE(Json::parseFromStream(builder, stream, &json, &errors));
  const auto reading = routes::readRemoteLink(json);
  REQUIRE(reading.state.has_value());
  return reading.state.value_or(routes::RemoteLinkState{});
}
}

TEST_CASE("the link reads up when connected, connecting before any failed attempt, down after one")
{
  CHECK(tunnel::link::linkOf({.connected = true, .attempts = 0}) == routes::RemoteLink::Up);
  CHECK(tunnel::link::linkOf({.connected = false, .attempts = 0}) == routes::RemoteLink::Connecting);
  CHECK(tunnel::link::linkOf({.connected = false, .attempts = 3}) == routes::RemoteLink::Down);
}

TEST_CASE("the tracker moves since only when the link changes, so retries while down change nothing")
{
  tunnel::link::LinkStatusTracker tracker(kStart);
  CHECK(tracker.link() == routes::RemoteLink::Connecting);
  CHECK(tracker.since() == kStart);
  CHECK_FALSE(tracker.observe({.connected = false, .attempts = 0}, kStart + 1));
  CHECK(tracker.observe({.connected = false, .attempts = 1}, kStart + 2));
  CHECK(tracker.link() == routes::RemoteLink::Down);
  CHECK(tracker.since() == kStart + 2);
  CHECK_FALSE(tracker.observe({.connected = false, .attempts = 2}, kStart + 9));
  CHECK_FALSE(tracker.observe({.connected = false, .attempts = 7}, kStart + 60));
  CHECK(tracker.since() == kStart + 2);
  CHECK(tracker.observe({.connected = true, .attempts = 0}, kStart + 61));
  CHECK(tracker.link() == routes::RemoteLink::Up);
  CHECK(tracker.since() == kStart + 61);
}

TEST_CASE("the published domain is the device domain suffix without its leading dot")
{
  CHECK(tunnel::link::deviceDomainOf(tunnel::link::kDeviceDomainSuffix) == "argus-tunnel.lynk.page");
  CHECK(tunnel::link::deviceDomainOf("argus-tunnel.lynk.page") == "argus-tunnel.lynk.page");
  CHECK_FALSE(tunnel::link::deviceDomainOf(".localhost").has_value());
  CHECK_FALSE(tunnel::link::deviceDomainOf("").has_value());
}

TEST_CASE("the first tick publishes the state, and the contract reads it back")
{
  Harness harness;
  harness.tickAt(kStart);
  REQUIRE(harness.sent.size() == 1);
  const auto state = decoded(harness.sent.front());
  CHECK(state.homeId == kHomeId);
  CHECK(state.domain == "argus-tunnel.lynk.page");
  CHECK(state.link == routes::RemoteLink::Connecting);
  CHECK(state.since == kStart);
}

TEST_CASE("a change is published at once, and a steady state is published again every 30 seconds")
{
  Harness harness;
  harness.tickAt(kStart);
  harness.tickAt(kStart + 5);
  CHECK(harness.sent.size() == 1);
  harness.sample = {.connected = true, .attempts = 0};
  harness.tickAt(kStart + 10);
  REQUIRE(harness.sent.size() == 2);
  CHECK(decoded(harness.sent.back()).link == routes::RemoteLink::Up);
  CHECK(decoded(harness.sent.back()).since == kStart + 10);
  harness.tickAt(kStart + 39);
  CHECK(harness.sent.size() == 2);
  harness.tickAt(kStart + 40);
  REQUIRE(harness.sent.size() == 3);
  CHECK(decoded(harness.sent.back()).link == routes::RemoteLink::Up);
  CHECK(decoded(harness.sent.back()).since == kStart + 10);
}

TEST_CASE("a state the bus did not take is tried again on the next tick")
{
  Harness harness;
  harness.busUp = false;
  harness.tickAt(kStart);
  harness.sample = {.connected = false, .attempts = 1};
  harness.tickAt(kStart + 1);
  CHECK(harness.sent.empty());
  harness.busUp = true;
  harness.tickAt(kStart + 2);
  REQUIRE(harness.sent.size() == 1);
  CHECK(decoded(harness.sent.front()).link == routes::RemoteLink::Down);
  CHECK(decoded(harness.sent.front()).since == kStart + 1);
  harness.tickAt(kStart + 3);
  CHECK(harness.sent.size() == 1);
}
