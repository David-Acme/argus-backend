#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

enum class MessageDirection : uint8_t
{
  Out = 0,
  In
};

inline std::string messageDirectionToString(MessageDirection direction)
{
  switch (direction) {
    case MessageDirection::Out:
      return "out";
    case MessageDirection::In:
      return "in";
  }
  return "in";
}

inline std::optional<MessageDirection> messageDirectionFromString(std::string_view value)
{
  if (value == "out")
    return MessageDirection::Out;
  if (value == "in")
    return MessageDirection::In;
  return std::nullopt;
}
