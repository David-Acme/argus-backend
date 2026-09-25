#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <config/config-service.hxx>
#include <cstdint>
#include <doctest/doctest.h>
#include <mdns/mdns-service.hxx>
#include <string>
#include <utility>
#include <vector>

namespace
{

MdnsInstance routeInstance(std::string path, uint16_t port, bool tls)
{
  std::vector<std::pair<std::string, std::string>> txt;
  txt.emplace_back("path", path);
  if (tls)
    txt.emplace_back("https", "true");
  return MdnsInstance{.serviceType = "_argus-route._tcp",
                      .path = std::move(path),
                      .port = port,
                      .txt = std::move(txt)};
}

}

TEST_CASE("advertising disabled answers true and advertises nothing")
{
  ConfigService::setRuntimeString("mdns.enabled", "false");
  MdnsService service({routeInstance("camera", 7026, true)});

  CHECK(service.initialize());
  CHECK_FALSE(service.isAdvertising());
  CHECK_FALSE(service.health()["advertising"].asBool());

  service.shutdown();
  service.shutdown();
  CHECK_FALSE(service.isAdvertising());
}

TEST_CASE("every announced instance is reported with its own records")
{
  ConfigService::setRuntimeString("mdns.enabled", "false");
  ConfigService::setRuntimeString("mdns.name", "Argus");
  MdnsService service({routeInstance("camera", 7026, true),
                       MdnsInstance{.serviceType = "_argus._tcp",
                                    .path = {},
                                    .port = 7024,
                                    .txt = {}}});

  const Json::Value instances = service.health()["instances"];
  REQUIRE(instances.size() == 2);

  CHECK(instances[0]["serviceType"].asString() == "_argus-route._tcp");
  CHECK(instances[0]["path"].asString() == "camera");
  CHECK(instances[0]["port"].asInt() == 7026);
  CHECK(instances[0]["instance"].asString() ==
        "Argus-camera._argus-route._tcp.local.");
  CHECK(instances[0]["txt"]["path"].asString() == "camera");
  CHECK(instances[0]["txt"]["https"].asString() == "true");

  CHECK(instances[1]["serviceType"].asString() == "_argus._tcp");
  CHECK(instances[1]["path"].asString().empty());
  CHECK(instances[1]["port"].asInt() == 7024);
  CHECK(instances[1]["instance"].asString() == "Argus._argus._tcp.local.");
  CHECK(instances[1]["txt"].empty());
}

TEST_CASE("a plain instance carries no https key in its txt")
{
  ConfigService::setRuntimeString("mdns.enabled", "false");
  MdnsService service({routeInstance("sync", 7025, false)});

  const Json::Value txt = service.health()["instances"][0]["txt"];
  CHECK(txt["path"].asString() == "sync");
  CHECK_FALSE(txt.isMember("https"));
}

TEST_CASE("a configured address is the one advertised instead of the host's")
{
  ConfigService::setRuntimeString("mdns.enabled", "true");
  ConfigService::setRuntimeString("mdns.address", "192.168.7.11");

  MdnsService service({routeInstance("camera", 7026, true)});

  CHECK(service.initialize());
  CHECK(service.health()["address"].asString() == "192.168.7.11");

  const Json::Value addresses = service.health()["addresses"];
  REQUIRE(addresses.size() == 1);
  CHECK(addresses[0].asString() == "192.168.7.11");

  service.shutdown();
}

TEST_CASE("a configured IPv6 address is advertised on its own family")
{
  ConfigService::setRuntimeString("mdns.enabled", "true");
  ConfigService::setRuntimeString("mdns.address", "fd00::11");

  MdnsService service({routeInstance("camera", 7026, true)});

  CHECK(service.initialize());

  const Json::Value addresses = service.health()["addresses"];
  REQUIRE(addresses.size() == 1);
  CHECK(addresses[0].asString() == "fd00::11");

  service.shutdown();
}

TEST_CASE("an address that is not an IP falls back to the host's interfaces")
{
  ConfigService::setRuntimeString("mdns.enabled", "true");
  ConfigService::setRuntimeString("mdns.address", "argus.local");

  MdnsService service({routeInstance("camera", 7026, true)});

  CHECK(service.initialize());
  CHECK(service.health()["address"].asString() == "argus.local");

  const Json::Value addresses = service.health()["addresses"];
  for (const Json::Value& address : addresses)
    CHECK(address.asString() != "argus.local");

  service.shutdown();
}

TEST_CASE("an unset address leaves the host's interfaces as the announcement")
{
  ConfigService::setRuntimeString("mdns.enabled", "true");
  ConfigService::setRuntimeString("mdns.address", "");

  MdnsService service({routeInstance("camera", 7026, true)});

  CHECK(service.initialize());
  CHECK(service.health()["address"].asString().empty());

  service.shutdown();
}

TEST_CASE("an empty name keeps the default the certificate's SAN can carry")
{
  ConfigService::setRuntimeString("mdns.enabled", "false");
  ConfigService::setRuntimeString("mdns.name", "");

  MdnsService service({routeInstance("auth", 7042, true)});
  CHECK(service.health()["name"].asString() == "Argus");
}

TEST_CASE("nothing to announce is not a failure")
{
  ConfigService::setRuntimeString("mdns.enabled", "true");
  MdnsService service({});

  CHECK(service.initialize());
  CHECK_FALSE(service.isAdvertising());

  service.shutdown();
}

TEST_CASE("an instance without a service type is not advertisable")
{
  ConfigService::setRuntimeString("mdns.enabled", "true");
  MdnsService service({MdnsInstance{.serviceType = {},
                                    .path = "camera",
                                    .port = 7026,
                                    .txt = {}}});

  CHECK(service.initialize());
  CHECK_FALSE(service.isAdvertising());

  service.shutdown();
}

TEST_CASE("a name longer than a label is clamped and keeps the route apart")
{
  ConfigService::setRuntimeString("mdns.enabled", "false");
  ConfigService::setRuntimeString("mdns.name", std::string(80, 'a'));

  MdnsService service({routeInstance("camera", 7026, true),
                       routeInstance("guard", 7039, true)});

  const std::string camera =
      service.health()["instances"][0]["instance"].asString();
  const std::string guard =
      service.health()["instances"][1]["instance"].asString();
  const std::string type = "._argus-route._tcp.local.";

  CHECK(camera == std::string(56, 'a') + "-camera" + type);
  CHECK(guard == std::string(57, 'a') + "-guard" + type);
  CHECK(camera.size() == 63 + type.size());
  CHECK(guard.size() == 63 + type.size());
  CHECK(camera != guard);
}
