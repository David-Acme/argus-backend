#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "tunnel-harness.hxx"

#include <doctest/doctest.h>

#include <server/health-extras.hxx>

#include <algorithm>
#include <string>
#include <vector>

using namespace tunnel;
using namespace tunnel::test;

namespace
{

std::vector<std::string> fieldNames(const HealthStatus& status)
{
  std::vector<std::string> names;
  names.reserve(status.extras.size());
  for (const auto& [key, provider] : status.extras)
    names.push_back(key);
  return names;
}

Json::Value valueOf(const HealthStatus& status, const std::string& key)
{
  for (const auto& [name, provider] : status.extras)
    if (name == key)
      return provider();
  return Json::Value();
}

bool has(const HealthStatus& status, const std::string& key)
{
  const auto names = fieldNames(status);
  return std::find(names.begin(), names.end(), key) != names.end();
}

} // namespace

TEST_CASE("relay and client /health carry their frozen field sets")
{
  Harness harness{HarnessOptions{}};
  REQUIRE(harness.start());

  const HealthStatus relayStatus = relayHealthStatus(*harness.relay);
  const HealthStatus clientStatus = clientHealthStatus(*harness.client);

  CHECK(relayStatus.serviceName == "argus-relay");
  CHECK(clientStatus.serviceName == "argus-tunnel-client");

  const std::vector<std::string> relayFields = {
      "homeConnected", "activeStreams", "pushQueued",
      "pushReceived",  "pushDropped",   "pushForwarded"};
  const std::vector<std::string> clientFields = {
      "homeConnected", "activeStreams", "pushQueued", "pushReceived",
      "pushDropped"};

  CHECK(fieldNames(relayStatus) == relayFields);
  CHECK(fieldNames(clientStatus) == clientFields);

  // pushForwarded is the relay's alone: the client never forwards a push on.
  CHECK(has(relayStatus, "pushForwarded"));
  CHECK_FALSE(has(clientStatus, "pushForwarded"));

  CHECK(valueOf(relayStatus, "homeConnected").isBool());
  CHECK(valueOf(relayStatus, "activeStreams").isInt());
  CHECK(valueOf(relayStatus, "pushForwarded").isIntegral());
  CHECK(valueOf(clientStatus, "homeConnected").isBool());
  CHECK(valueOf(clientStatus, "activeStreams").isInt());
  CHECK(valueOf(clientStatus, "pushDropped").isIntegral());
}

TEST_CASE("health providers sample live state, not boot state")
{
  Harness harness{HarnessOptions{}};
  REQUIRE(harness.start());

  // Taken once, before any stream exists, and never rebuilt below.
  const HealthStatus relayStatus = relayHealthStatus(*harness.relay);
  const HealthStatus clientStatus = clientHealthStatus(*harness.client);

  CHECK(valueOf(relayStatus, "homeConnected").asBool());
  CHECK(valueOf(clientStatus, "homeConnected").asBool());
  CHECK(valueOf(relayStatus, "activeStreams").asInt() == 0);

  auto device = connectTestPeer({.loop = harness.loop,
                                 .ip = "127.0.0.1",
                                 .port = harness.relay->devicePort()});
  REQUIRE(waitFor([device] { return device->connected.load(); }, 5000));

  // A provider frozen at construction would still report zero here.
  CHECK(waitFor(
      [&relayStatus] {
        return valueOf(relayStatus, "activeStreams").asInt() == 1;
      },
      5000));
}

TEST_CASE("the /health envelope keeps the shared shape with the extras merged")
{
  Harness harness{HarnessOptions{}};
  REQUIRE(harness.start());

  const HealthStatus status = relayHealthStatus(*harness.relay);
  Json::Value payload = HealthController::info(status.serviceName, 1.5);
  for (const auto& [key, provider] : status.extras)
    payload[key] = provider();

  CHECK(payload["service"].asString() == "argus-relay");
  CHECK(payload["uptimeSeconds"].asDouble() == doctest::Approx(1.5));
  CHECK(payload.isMember("pushForwarded"));
  CHECK(payload.size() == 8);
}
