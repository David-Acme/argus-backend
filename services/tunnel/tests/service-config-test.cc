#include <server/service-config.hxx>
#include <config/config-service.hxx>

#include <doctest/doctest.h>

#include <chrono>
#include <string>

using namespace tunnel;

TEST_CASE("a missing stream_idle_seconds yields the struct default")
{
  const ClientConfig config = ClientConfig::resolve();
  CHECK(config.tunnel.limits.idleTimeout == std::chrono::seconds(300));
}

TEST_CASE("an explicit stream_idle_seconds overrides the default")
{
  ConfigService::setRuntimeString("tunnel.stream_idle_seconds", "5");
  const ClientConfig config = ClientConfig::resolve();
  CHECK(config.tunnel.limits.idleTimeout == std::chrono::seconds(5));

  ConfigService::setRuntimeString(
      "tunnel.stream_idle_seconds",
      std::to_string(TunnelMux::Limits{}.idleTimeout.count()));
}

TEST_CASE("the client dials argus-auth's loopback tunnel listener by default")
{
  const ClientConfig config = ClientConfig::resolve();
  CHECK(config.tunnel.gatewayHost == "127.0.0.1");
  CHECK(config.tunnel.gatewayPort == 7142);
  CHECK(config.tunnel.reconnectMaxWaitMs == 60000);
}

TEST_CASE("a per-address stream quota comes from the config")
{
  CHECK(ClientConfig::resolve().tunnel.limits.maxStreamsPerIp == 32);
  ConfigService::setRuntimeString("tunnel.max_streams_per_ip", "4");
  CHECK(RelayConfig::resolve().relay.limits.maxStreamsPerIp == 4);
  ConfigService::setRuntimeString("tunnel.max_streams_per_ip", "32");
}

TEST_CASE("each [link.services] entry gives its service a listener")
{
  const ClientConfig unconfigured = ClientConfig::resolve();
  CHECK(unconfigured.link.serviceListeners.empty());
  CHECK(unconfigured.link.deviceDomainSuffix == ".argus-tunnel.lynk.page");

  ConfigService::setRuntimeString("link.services.sync", "7025");
  ConfigService::setRuntimeString("link.services.auth", "7142");
  ConfigService::setRuntimeString("link.services.media", "7026");
  const ClientConfig configured = ClientConfig::resolve();
  CHECK(configured.link.serviceListeners.size() == 2);
  CHECK(configured.link.serviceListeners.at(link::HomeService::Sync) == 7025);
  CHECK(configured.link.serviceListeners.at(link::HomeService::Auth) == 7142);

  ConfigService::setRuntimeString("link.services.sync", "0");
  ConfigService::setRuntimeString("link.services.auth", "0");
  ConfigService::setRuntimeString("link.services.media", "0");
  CHECK(ClientConfig::resolve().link.serviceListeners.empty());
}

TEST_CASE("a tunnel secret must carry at least 32 bytes")
{
  CHECK(tunnelSecretProblem("").has_value());
  CHECK(tunnelSecretProblem("short-secret").has_value());
  CHECK(tunnelSecretProblem(std::string(31, 'a')).has_value());
  CHECK(tunnelSecretProblem("CHANGE_ME_TUNNEL_SECRET_SAME_ON_BOTH_SIDES").has_value());
  CHECK_FALSE(tunnelSecretProblem(std::string(32, 'a')).has_value());
  CHECK_FALSE(tunnelSecretProblem(std::string(64, 'f')).has_value());
}
