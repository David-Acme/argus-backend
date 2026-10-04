#include "rtc-naming.hxx"

#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <stdexcept>

namespace rtc_naming
{

namespace
{

constexpr std::size_t kUserCallIdHexChars = 32;
constexpr std::size_t kMaxProactiveDigits = 19;

bool allOf(std::string_view text, auto predicate)
{
  return !text.empty() && std::ranges::all_of(text, predicate);
}

}

std::string mintUserCallId()
{
  std::array<unsigned char, kUserCallIdHexChars / 2> bytes{};
  if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1)
    throw std::runtime_error("RAND_bytes failed");
  constexpr std::string_view hex = "0123456789abcdef";
  std::string id(kUserCallPrefix);
  id.reserve(kUserCallPrefix.size() + kUserCallIdHexChars);
  for (const unsigned char byte : bytes) {
    id.push_back(hex[byte >> 4U]);
    id.push_back(hex[byte & 0x0FU]);
  }
  return id;
}

CallKind callKindOf(std::string_view callId)
{
  if (callId.starts_with(kUserCallPrefix)) {
    const auto rest = callId.substr(kUserCallPrefix.size());
    return rest.size() == kUserCallIdHexChars &&
                   allOf(rest, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); })
               ? CallKind::User
               : CallKind::Invalid;
  }
  if (callId.starts_with(kProactiveCallPrefix)) {
    const auto rest = callId.substr(kProactiveCallPrefix.size());
    return !rest.empty() && rest.size() <= kMaxProactiveDigits && rest.front() != '0' &&
                   allOf(rest, [](char c) { return c >= '0' && c <= '9'; })
               ? CallKind::Proactive
               : CallKind::Invalid;
  }
  return CallKind::Invalid;
}

std::string roomPrefixOf(int64_t userId)
{
  return "u" + std::to_string(userId) + ".";
}

std::string roomOf(int64_t userId, std::string_view callId)
{
  return roomPrefixOf(userId) + std::string(callId);
}

std::string userIdentityPrefixOf(int64_t userId)
{
  return "user:" + std::to_string(userId) + ":";
}

std::string userIdentityOf(int64_t userId, std::string_view sessionId)
{
  return userIdentityPrefixOf(userId) + std::string(sessionId);
}

std::string hostnameOf(std::string_view hostHeader)
{
  constexpr std::size_t kMaxHostChars = 253;
  if (hostHeader.starts_with('[')) {
    const auto end = hostHeader.find(']');
    if (end == std::string_view::npos || end < 2)
      return {};
    const auto address = hostHeader.substr(1, end - 1);
    const bool valid = allOf(address, [](char c) {
      return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F') || c == ':' || c == '.';
    });
    return valid ? std::string(hostHeader.substr(0, end + 1)) : std::string();
  }
  const auto host = hostHeader.substr(0, hostHeader.find(':'));
  const bool valid = host.size() <= kMaxHostChars && allOf(host, [](char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '.' || c == '-';
  });
  return valid ? std::string(host) : std::string();
}

std::string publicUrlOf(const PublicUrlInput& input)
{
  if (!input.configured.empty())
    return std::string(input.configured);
  std::string host = hostnameOf(input.host);
  if (host.empty())
    host = "argus.local";
  return "wss://" + host + ":" + std::to_string(input.port);
}

}
