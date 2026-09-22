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

// The event id names the event: a transition is discriminated by its own
// payload, so replaying it is the same id while a record that moves again — or
// returns to a state it already held — is a new one. 32 hex digits keep the
// JetStream MsgId inside the broker's header budget.
[[nodiscard]] inline std::string eventId(const ChangeOutboxKeyInput& input)
{
  std::string key(input.table);
  key += '|';
  key += std::to_string(input.recordId);
  key += '|';
  key.append(input.discriminator);
  return "productivity-change:" + argus::hash::sha256Hex(key).substr(0, 32);
}

// The canonical payload, so one id reaching the outbox twice under two
// fingerprints is two writers racing over one prior state.
[[nodiscard]] inline std::string fingerprintJson(std::string_view payload)
{
  return argus::hash::sha256Hex(payload);
}

[[nodiscard]] inline std::string fingerprint(const Json::Value& payload)
{
  return fingerprintJson(json_util::toString(payload));
}

} // namespace change_outbox_key
