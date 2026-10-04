#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <auth/details/proxy-allowlist.hxx>

#include <string_view>

namespace
{
bool trusts(const proxy_allowlist::MembershipInput& input)
{
  return proxy_allowlist::contains(input);
}
}

TEST_CASE("an empty allowlist trusts no peer, loopback included")
{
  CHECK_FALSE(trusts({.configured = "", .address = "127.0.0.1"}));
  CHECK_FALSE(trusts({.configured = "", .address = "::1"}));
  CHECK_FALSE(trusts({.configured = "  ", .address = "10.0.0.1"}));
}

TEST_CASE("an exact address trusts that peer alone")
{
  CHECK(trusts({.configured = "172.19.0.1", .address = "172.19.0.1"}));
  CHECK_FALSE(trusts({.configured = "172.19.0.1", .address = "172.19.0.2"}));
  CHECK(trusts({.configured = "10.0.0.9, 172.19.0.1", .address = "172.19.0.1"}));
  CHECK(trusts({.configured = "::1", .address = "::1"}));
  CHECK_FALSE(trusts({.configured = "::1", .address = "127.0.0.1"}));
}

TEST_CASE("a CIDR trusts the peers inside its prefix")
{
  CHECK(trusts({.configured = "172.19.0.0/24", .address = "172.19.0.200"}));
  CHECK_FALSE(trusts({.configured = "172.19.0.0/24", .address = "172.19.1.1"}));
  CHECK(trusts({.configured = "10.0.0.0/8", .address = "10.255.3.4"}));
  CHECK(trusts({.configured = "10.1.2.3/31", .address = "10.1.2.2"}));
  CHECK_FALSE(trusts({.configured = "10.1.2.3/31", .address = "10.1.2.4"}));
  CHECK(trusts({.configured = "0.0.0.0/0", .address = "8.8.8.8"}));
  CHECK(trusts({.configured = "fd00::/8", .address = "fd12:3456::1"}));
  CHECK_FALSE(trusts({.configured = "fd00::/8", .address = "fe80::1"}));
}

TEST_CASE("an IPv4-mapped peer matches its IPv4 entry")
{
  CHECK(trusts({.configured = "172.19.0.1", .address = "::ffff:172.19.0.1"}));
  CHECK(trusts({.configured = "172.19.0.0/16", .address = "::ffff:172.19.4.4"}));
}

TEST_CASE("malformed entries and peers trust nothing")
{
  CHECK_FALSE(trusts({.configured = "172.19.0.0/33", .address = "172.19.0.1"}));
  CHECK_FALSE(trusts({.configured = "172.19.0.0/", .address = "172.19.0.1"}));
  CHECK_FALSE(trusts({.configured = "172.19.0.0/x", .address = "172.19.0.1"}));
  CHECK_FALSE(trusts({.configured = "argus-relay", .address = "172.19.0.1"}));
  CHECK_FALSE(trusts({.configured = "172.19.0.1", .address = "not-an-address"}));
  CHECK_FALSE(trusts({.configured = "172.19.0.1", .address = ""}));
  CHECK(trusts({.configured = "bogus, 172.19.0.1", .address = "172.19.0.1"}));
}
