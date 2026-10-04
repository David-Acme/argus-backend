#pragma once

#include <cstdint>
#include <string>

enum class SessionPlatform : std::uint8_t
{
  Unknown = 0,
  Android,
  Ios,
  Desktop,
  Web
};

inline std::string sessionPlatformToString(SessionPlatform platform)
{
  switch (platform) {
    case SessionPlatform::Android:
      return "android";
    case SessionPlatform::Ios:
      return "ios";
    case SessionPlatform::Desktop:
      return "desktop";
    case SessionPlatform::Web:
      return "web";
    case SessionPlatform::Unknown:
      return "unknown";
  }
  return "unknown";
}

inline SessionPlatform sessionPlatformFromString(const std::string& value)
{
  if (value == "android")
    return SessionPlatform::Android;
  if (value == "ios")
    return SessionPlatform::Ios;
  if (value == "desktop")
    return SessionPlatform::Desktop;
  if (value == "web")
    return SessionPlatform::Web;
  return SessionPlatform::Unknown;
}
