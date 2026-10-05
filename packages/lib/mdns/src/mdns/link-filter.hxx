#pragma once

#include <arpa/inet.h>
#include <ifaddrs.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

namespace mdns_link
{

struct LinkNetwork
{
  int family{AF_UNSPEC};
  std::array<std::uint8_t, 16> address{};
  std::array<std::uint8_t, 16> mask{};
};

struct SourceAddress
{
  int family{AF_UNSPEC};
  std::array<std::uint8_t, 16> bytes{};
};

inline SourceAddress sourceOf(const struct sockaddr* from)
{
  SourceAddress source;
  if (from == nullptr)
    return source;
  if (from->sa_family == AF_INET) {
    struct sockaddr_in v4{};
    std::memcpy(&v4, from, sizeof(v4));
    source.family = AF_INET;
    std::memcpy(source.bytes.data(), &v4.sin_addr, 4);
    return source;
  }
  if (from->sa_family == AF_INET6) {
    struct sockaddr_in6 v6{};
    std::memcpy(&v6, from, sizeof(v6));
    if (IN6_IS_ADDR_V4MAPPED(&v6.sin6_addr)) {
      source.family = AF_INET;
      std::memcpy(source.bytes.data(), v6.sin6_addr.s6_addr + 12, 4);
      return source;
    }
    source.family = AF_INET6;
    std::memcpy(source.bytes.data(), v6.sin6_addr.s6_addr, 16);
  }
  return source;
}

inline bool within(const LinkNetwork& network, const SourceAddress& source)
{
  if (network.family != source.family)
    return false;
  const std::size_t width = source.family == AF_INET ? 4 : 16;
  for (std::size_t index = 0; index < width; ++index) {
    if ((network.address[index] & network.mask[index]) !=
        (source.bytes[index] & network.mask[index]))
      return false;
  }
  return true;
}

inline bool alwaysOnLink(const SourceAddress& source)
{
  if (source.family == AF_INET)
    return source.bytes[0] == 127 ||
           (source.bytes[0] == 169 && source.bytes[1] == 254);
  if (source.family == AF_INET6) {
    const bool loopback =
        std::all_of(source.bytes.begin(), source.bytes.end() - 1,
                    [](std::uint8_t byte) { return byte == 0; }) &&
        source.bytes[15] == 1;
    const bool linkLocal =
        source.bytes[0] == 0xfe && (source.bytes[1] & 0xc0) == 0x80;
    return loopback || linkLocal;
  }
  return false;
}

inline bool onLink(const SourceAddress& source,
                   const std::vector<LinkNetwork>& networks)
{
  if (source.family == AF_UNSPEC)
    return false;
  if (alwaysOnLink(source))
    return true;
  return std::ranges::any_of(networks, [&source](const LinkNetwork& network) {
    return within(network, source);
  });
}

inline std::vector<LinkNetwork> interfaceNetworks()
{
  std::vector<LinkNetwork> networks;
  struct ifaddrs* raw = nullptr;
  if (getifaddrs(&raw) < 0)
    return networks;
  const std::unique_ptr<struct ifaddrs, decltype(&freeifaddrs)> list(raw,
                                                                    freeifaddrs);
  for (const struct ifaddrs* entry = list.get(); entry != nullptr;
       entry = entry->ifa_next) {
    if (entry->ifa_addr == nullptr || entry->ifa_netmask == nullptr)
      continue;
    const SourceAddress address = sourceOf(entry->ifa_addr);
    const SourceAddress mask = sourceOf(entry->ifa_netmask);
    if (address.family == AF_UNSPEC || entry->ifa_addr->sa_family !=
                                           entry->ifa_netmask->sa_family)
      continue;
    networks.push_back(
        {.family = address.family, .address = address.bytes, .mask = mask.bytes});
  }
  return networks;
}

}
