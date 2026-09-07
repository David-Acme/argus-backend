#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <server/service-config.hxx>
#include <shared/services/config-service/config-service.hxx>

#include <doctest/doctest.h>

#include <chrono>

using namespace tunnel;

TEST_CASE("a missing stream_idle_seconds yields the struct default")
{
  // No config file is loaded in a fresh process, so every [tunnel] key is
  // missing here.
  const ClientConfig config = ClientConfig::resolve();
  CHECK(config.tunnel.limits.idleTimeout == std::chrono::seconds(300));
}

TEST_CASE("an explicit stream_idle_seconds overrides the default")
{
  ConfigService::setRuntimeString("tunnel.stream_idle_seconds", "5");
  const ClientConfig config = ClientConfig::resolve();
  CHECK(config.tunnel.limits.idleTimeout == std::chrono::seconds(5));
}
