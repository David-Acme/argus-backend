#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/config-service.hxx>
#include <mdns/mdns-service.hxx>

TEST_CASE("advertising disabled answers true and advertises nothing")
{
  ConfigService::setRuntimeString("mdns.enabled", "false");
  MdnsService service;

  CHECK(service.initialize());
  CHECK_FALSE(service.isAdvertising());
  CHECK_FALSE(service.health()["advertising"].asBool());

  service.shutdown();
  service.shutdown();
  CHECK_FALSE(service.isAdvertising());
}

TEST_CASE("the mdns.* keys are what the health report shows")
{
  ConfigService::setRuntimeString("mdns.enabled", "false");
  ConfigService::setRuntimeString("mdns.name", "argus-lab");
  ConfigService::setRuntimeString("mdns.service_type", "_argus-lab._tcp");
  ConfigService::setRuntimeString("mdns.port", "7099");

  MdnsService service;
  const Json::Value health = service.health();

  CHECK(health["name"].asString() == "argus-lab");
  CHECK(health["serviceType"].asString() == "_argus-lab._tcp");
  CHECK(health["port"].asInt() == 7099);
}

TEST_CASE("a port outside the range keeps the default instead of announcing it")
{
  ConfigService::setRuntimeString("mdns.enabled", "false");
  ConfigService::setRuntimeString("mdns.port", "70000");

  MdnsService service;
  CHECK(service.health()["port"].asInt() == 7024);
}

TEST_CASE("an empty value keeps the default the certificate's SAN can carry")
{
  ConfigService::setRuntimeString("mdns.enabled", "false");
  ConfigService::setRuntimeString("mdns.name", "");
  ConfigService::setRuntimeString("mdns.service_type", "");

  MdnsService service;
  CHECK(service.health()["name"].asString() == "Argus");
  CHECK(service.health()["serviceType"].asString() == "_argus._tcp");
}
