#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <text/json-util.hxx>
#include <text/sha256.hxx>

namespace change_outbox_key
{

inline constexpr std::string_view kActionPrefix = "auth-action:";
inline constexpr std::string_view kSessionPrefix = "auth-session:";

[[nodiscard]] inline std::string
prefixedMsgId(std::string_view prefix,
              const std::array<unsigned char, 16>& entropy)
{
  static constexpr std::string_view kHex = "0123456789abcdef";
  std::string msgId(prefix);
  msgId.reserve(msgId.size() + entropy.size() * 2);
  for (const auto byte : entropy) {
    msgId.push_back(kHex[byte >> 4U]);
    msgId.push_back(kHex[byte & 0x0FU]);
  }
  return msgId;
}

[[nodiscard]] inline std::string
actionMsgId(const std::array<unsigned char, 16>& entropy)
{
  return prefixedMsgId(kActionPrefix, entropy);
}

[[nodiscard]] inline std::string
sessionMsgId(const std::array<unsigned char, 16>& entropy)
{
  return prefixedMsgId(kSessionPrefix, entropy);
}

[[nodiscard]] inline std::string legacyActionMsgId(int64_t id)
{
  return std::string(kActionPrefix) + std::to_string(id);
}

[[nodiscard]] inline std::string fingerprintJson(std::string_view payload)
{
  return argus::hash::sha256Hex(payload);
}

}
