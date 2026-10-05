#pragma once

#include <cstdint>
#include <string>

enum class SessionOrigin : std::uint8_t
{
  Unknown = 0,
  Lan,
  Tunnel,
  Loopback,
  External
};

inline std::string sessionOriginToString(SessionOrigin origin)
{
  switch (origin) {
    case SessionOrigin::Lan:
      return "lan";
    case SessionOrigin::Tunnel:
      return "tunnel";
    case SessionOrigin::Loopback:
      return "loopback";
    case SessionOrigin::External:
      return "external";
    case SessionOrigin::Unknown:
      return "unknown";
  }
  return "unknown";
}

inline SessionOrigin sessionOriginFromString(const std::string& value)
{
  if (value == "lan")
    return SessionOrigin::Lan;
  if (value == "tunnel")
    return SessionOrigin::Tunnel;
  if (value == "loopback")
    return SessionOrigin::Loopback;
  if (value == "external")
    return SessionOrigin::External;
  return SessionOrigin::Unknown;
}
