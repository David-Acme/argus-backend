#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <text/json-util.hxx>
#include <text/sha256.hxx>

namespace change_outbox_key
{

[[nodiscard]] inline std::string actionMsgId(int64_t id)
{
  return "auth-action:" + std::to_string(id);
}

[[nodiscard]] inline std::string fingerprintJson(std::string_view payload)
{
  return argus::hash::sha256Hex(payload);
}

}
