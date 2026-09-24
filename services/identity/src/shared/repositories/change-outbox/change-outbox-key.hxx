#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <json/value.h>
#include <text/json-util.hxx>
#include <text/sha256.hxx>

namespace change_outbox_key
{

struct ChangeOutboxKeyInput
{
  std::string_view table;
  int64_t recordId{0};
  std::string_view discriminator;
};

[[nodiscard]] inline std::string eventId(const ChangeOutboxKeyInput& input)
{
  std::string key(input.table);
  key += '|';
  key += std::to_string(input.recordId);
  key += '|';
  key.append(input.discriminator);
  return "identity-change:" + argus::hash::sha256Hex(key).substr(0, 32);
}

[[nodiscard]] inline std::string actionMsgId(int64_t id)
{
  return "identity-action:" + std::to_string(id);
}

[[nodiscard]] inline std::string fingerprintJson(std::string_view payload)
{
  return argus::hash::sha256Hex(payload);
}

[[nodiscard]] inline std::string fingerprint(const Json::Value& payload)
{
  return fingerprintJson(json_util::toString(payload));
}

}
