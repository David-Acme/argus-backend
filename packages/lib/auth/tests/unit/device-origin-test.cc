#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <auth/device-filter.hxx>

namespace
{
SessionOrigin originOf(std::string_view address)
{
  return DeviceFilter::classifyOrigin(
      {.viaTunnel = false,
       .address = address,
       .lanNetworks = DeviceFilter::kDefaultLanNetworks});
}
}

TEST_CASE("private and link-local addresses are the home network")
{
  CHECK(originOf("192.168.18.40") == SessionOrigin::Lan);
  CHECK(originOf("10.1.2.3") == SessionOrigin::Lan);
  CHECK(originOf("172.16.0.9") == SessionOrigin::Lan);
  CHECK(originOf("172.31.255.1") == SessionOrigin::Lan);
  CHECK(originOf("169.254.10.10") == SessionOrigin::Lan);
  CHECK(originOf("fd12:3456::1") == SessionOrigin::Lan);
  CHECK(originOf("fe80::1") == SessionOrigin::Lan);
  CHECK(originOf("::ffff:192.168.1.5") == SessionOrigin::Lan);
}

TEST_CASE("loopback, public and carrier-grade addresses are not the home "
          "network")
{
  CHECK(originOf("127.0.0.1") == SessionOrigin::Loopback);
  CHECK(originOf("::1") == SessionOrigin::Loopback);
  CHECK(originOf("8.8.8.8") == SessionOrigin::External);
  CHECK(originOf("172.32.0.1") == SessionOrigin::External);
  CHECK(originOf("100.64.0.1") == SessionOrigin::External);
  CHECK(originOf("2001:db8::1") == SessionOrigin::External);
}

TEST_CASE("an address that does not parse tells nothing")
{
  CHECK(originOf("") == SessionOrigin::Unknown);
  CHECK(originOf("not-an-ip") == SessionOrigin::Unknown);
}

TEST_CASE("the tunnel listener wins over any address")
{
  CHECK(DeviceFilter::classifyOrigin(
            {.viaTunnel = true,
             .address = "127.0.0.1",
             .lanNetworks = DeviceFilter::kDefaultLanNetworks}) ==
        SessionOrigin::Tunnel);
}

TEST_CASE("an owner-configured network list replaces the default")
{
  CHECK(DeviceFilter::classifyOrigin({.viaTunnel = false,
                                      .address = "100.64.3.4",
                                      .lanNetworks = "100.64.0.0/10"}) ==
        SessionOrigin::Lan);
  CHECK(DeviceFilter::classifyOrigin({.viaTunnel = false,
                                      .address = "192.168.1.4",
                                      .lanNetworks = "100.64.0.0/10"}) ==
        SessionOrigin::External);
}
