#pragma once

#include <arpa/inet.h>
#include <netinet/in.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace network_address
{

inline bool isLiteral(std::string_view host)
{
  const std::string text(host);
  in_addr v4{};
  in6_addr v6{};
  return ::inet_pton(AF_INET, text.c_str(), &v4) == 1 ||
         ::inet_pton(AF_INET6, text.c_str(), &v6) == 1;
}

inline bool isPrivateV4(const std::array<uint8_t, 4>& octets)
{
  const uint8_t first = octets[0];
  const uint8_t second = octets[1];
  return first == 10 || first == 127 || (first == 172 && second >= 16 && second <= 31) ||
         (first == 192 && second == 168) || (first == 169 && second == 254);
}

inline bool isPrivate(std::string_view host)
{
  if (host == "localhost")
    return true;
  const std::string text(host);
  in_addr v4{};
  if (::inet_pton(AF_INET, text.c_str(), &v4) == 1) {
    std::array<uint8_t, 4> octets{};
    const uint32_t address = ntohl(v4.s_addr);
    for (size_t i = 0; i < octets.size(); ++i)
      octets.at(i) = static_cast<uint8_t>((address >> (24 - 8 * i)) & 0xFFU);
    return isPrivateV4(octets);
  }
  in6_addr v6{};
  if (::inet_pton(AF_INET6, text.c_str(), &v6) != 1)
    return false;
  std::array<uint8_t, 16> bytes{};
  for (size_t i = 0; i < bytes.size(); ++i)
    bytes.at(i) = v6.s6_addr[i];
  const bool mapped = std::all_of(bytes.begin(), bytes.begin() + 10,
                                  [](uint8_t byte) { return byte == 0; }) &&
                      bytes[10] == 0xFFU && bytes[11] == 0xFFU;
  if (mapped)
    return isPrivateV4({bytes[12], bytes[13], bytes[14], bytes[15]});
  const bool loopback = std::all_of(bytes.begin(), bytes.end() - 1,
                                    [](uint8_t byte) { return byte == 0; }) &&
                        bytes[15] == 1;
  const bool uniqueLocal = (bytes[0] & 0xFEU) == 0xFCU;
  const bool linkLocal = bytes[0] == 0xFEU && (bytes[1] & 0xC0U) == 0x80U;
  return loopback || uniqueLocal || linkLocal;
}

inline std::string urlHost(std::string_view host)
{
  if (host.find(':') != std::string_view::npos)
    return "[" + std::string(host) + "]";
  return std::string(host);
}

}
