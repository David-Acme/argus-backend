#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

enum class CallMode : uint8_t
{
  Call = 0,
  Notify,
  Off
};

inline std::string callModeToString(CallMode mode)
{
  switch (mode) {
    case CallMode::Call:
      return "call";
    case CallMode::Notify:
      return "notify";
    case CallMode::Off:
      return "off";
  }
  return "notify";
}

inline std::optional<CallMode> callModeFromString(std::string_view value)
{
  if (value == "call")
    return CallMode::Call;
  if (value == "notify")
    return CallMode::Notify;
  if (value == "off")
    return CallMode::Off;
  return std::nullopt;
}
