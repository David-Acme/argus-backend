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

[[nodiscard]] inline std::string
actionMsgId(const std::array<unsigned char, 16>& entropy)
{
  static constexpr std::string_view kHex = "0123456789abcdef";
  std::string msgId(kActionPrefix);
  msgId.reserve(msgId.size() + entropy.size() * 2);
  for (const auto byte : entropy) {
    msgId.push_back(kHex[byte >> 4U]);
    msgId.push_back(kHex[byte & 0x0FU]);
  }
  return msgId;
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
