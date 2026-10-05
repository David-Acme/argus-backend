#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <mdns/link-filter.hxx>

#include <arpa/inet.h>
#include <netinet/in.h>

#include <string>
#include <vector>

namespace
{

struct sockaddr_storage v4(const char* text)
{
  struct sockaddr_storage storage{};
  auto* address = reinterpret_cast<struct sockaddr_in*>(&storage);
  address->sin_family = AF_INET;
  inet_pton(AF_INET, text, &address->sin_addr);
  return storage;
}

struct sockaddr_storage v6(const char* text)
{
  struct sockaddr_storage storage{};
  auto* address = reinterpret_cast<struct sockaddr_in6*>(&storage);
  address->sin6_family = AF_INET6;
  inet_pton(AF_INET6, text, &address->sin6_addr);
  return storage;
}

mdns_link::LinkNetwork network(const char* address, const char* mask)
{
  const auto host = v4(address);
  const auto netmask = v4(mask);
  const auto hostBytes =
      mdns_link::sourceOf(reinterpret_cast<const struct sockaddr*>(&host));
  const auto maskBytes =
      mdns_link::sourceOf(reinterpret_cast<const struct sockaddr*>(&netmask));
  return {.family = AF_INET, .address = hostBytes.bytes, .mask = maskBytes.bytes};
}

bool accepted(const struct sockaddr_storage& storage,
              const std::vector<mdns_link::LinkNetwork>& networks)
{
  return mdns_link::onLink(
      mdns_link::sourceOf(reinterpret_cast<const struct sockaddr*>(&storage)),
      networks);
}

}

TEST_CASE("a query from the host's own subnet is answered")
{
  const std::vector networks{network("192.168.1.20", "255.255.255.0")};
  CHECK(accepted(v4("192.168.1.77"), networks));
  CHECK(accepted(v6("::ffff:192.168.1.5"), networks));
}

TEST_CASE("a query from beyond the local link is ignored")
{
  const std::vector networks{network("192.168.1.20", "255.255.255.0")};
  CHECK_FALSE(accepted(v4("192.168.2.77"), networks));
  CHECK_FALSE(accepted(v4("8.8.8.8"), networks));
  CHECK_FALSE(accepted(v6("2001:db8::1"), networks));
  CHECK_FALSE(accepted(v4("10.0.0.1"), {}));
}

TEST_CASE("link-local and loopback sources are always on the link")
{
  CHECK(accepted(v6("fe80::1234"), {}));
  CHECK(accepted(v6("::1"), {}));
  CHECK(accepted(v4("169.254.10.1"), {}));
  CHECK(accepted(v4("127.0.0.1"), {}));
}

TEST_CASE("an unknown address family is never on the link")
{
  struct sockaddr_storage storage{};
  storage.ss_family = AF_UNIX;
  CHECK_FALSE(accepted(storage, {}));
  CHECK_FALSE(mdns_link::onLink(mdns_link::sourceOf(nullptr), {}));
}

TEST_CASE("the host's interfaces are read with their masks")
{
  const auto networks = mdns_link::interfaceNetworks();
  CHECK(accepted(v4("127.0.0.1"), networks));
}
