#pragma once

#include <auth/session-platform.hxx>
#include <cstddef>
#include <drogon/HttpRequest.h>
#include <optional>
#include <string>
#include <string_view>

struct ClientIdentity
{
  SessionPlatform platform{SessionPlatform::Unknown};
  std::string deviceName;
};

namespace client_identity
{
inline constexpr std::string_view kClientHeader = "X-Argus-Client";
inline constexpr std::string_view kDeviceHeader = "X-Argus-Device";
inline constexpr std::size_t kMaxDeviceNameChars = 64;

[[nodiscard]] std::optional<SessionPlatform>
platformOfStableUserAgent(std::string_view userAgent);

[[nodiscard]] bool isStableUserAgent(std::string_view userAgent);

[[nodiscard]] SessionPlatform platformOfClientHeader(std::string_view value);

[[nodiscard]] std::string decodeDeviceName(std::string_view encoded);

[[nodiscard]] ClientIdentity of(const drogon::HttpRequestPtr& req);
}
