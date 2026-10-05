#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

enum class RecipientMode : uint8_t
{
  Call = 0,
  Notify,
  Off
};

inline std::string recipientModeToString(RecipientMode mode)
{
  switch (mode) {
    case RecipientMode::Call:
      return "call";
    case RecipientMode::Notify:
      return "notify";
    case RecipientMode::Off:
      return "off";
  }
  return "off";
}

inline std::optional<RecipientMode>
recipientModeFromString(std::string_view value)
{
  if (value == "call")
    return RecipientMode::Call;
  if (value == "notify")
    return RecipientMode::Notify;
  if (value == "off")
    return RecipientMode::Off;
  return std::nullopt;
}
