#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace rtc_naming
{

inline constexpr std::string_view kAgentIdentity = "argus-voice";
inline constexpr std::string_view kUserCallPrefix = "rtc-";
inline constexpr std::string_view kProactiveCallPrefix = "call-";

enum class CallKind : uint8_t
{
  Invalid,
  User,
  Proactive
};

struct PublicUrlInput
{
  std::string_view configured;
  std::string_view host;
  uint16_t port{0};
};

[[nodiscard]] std::string mintUserCallId();
[[nodiscard]] CallKind callKindOf(std::string_view callId);
[[nodiscard]] std::string roomOf(int64_t userId, std::string_view callId);
[[nodiscard]] std::string roomPrefixOf(int64_t userId);
[[nodiscard]] std::string userIdentityOf(int64_t userId, std::string_view sessionId);
[[nodiscard]] std::string userIdentityPrefixOf(int64_t userId);
[[nodiscard]] std::string publicUrlOf(const PublicUrlInput& input);
[[nodiscard]] std::string hostnameOf(std::string_view hostHeader);

}
