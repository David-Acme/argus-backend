#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

enum class MessageKind : uint8_t
{
  Turn = 0,
  Relay,
  Reply,
  Visitor
};

inline std::string messageKindToString(MessageKind kind)
{
  switch (kind) {
    case MessageKind::Turn:
      return "turn";
    case MessageKind::Relay:
      return "relay";
    case MessageKind::Reply:
      return "reply";
    case MessageKind::Visitor:
      return "visitor";
  }
  return "turn";
}

inline std::optional<MessageKind> messageKindFromString(std::string_view value)
{
  if (value == "turn")
    return MessageKind::Turn;
  if (value == "relay")
    return MessageKind::Relay;
  if (value == "reply")
    return MessageKind::Reply;
  if (value == "visitor")
    return MessageKind::Visitor;
  return std::nullopt;
}
