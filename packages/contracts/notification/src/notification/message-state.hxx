#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

enum class MessageState : uint8_t
{
  Sent = 0,
  Delivered,
  Notified,
  Answered,
  Expired,
  Received
};

inline std::string messageStateToString(MessageState state)
{
  switch (state) {
    case MessageState::Sent:
      return "sent";
    case MessageState::Delivered:
      return "delivered";
    case MessageState::Notified:
      return "notified";
    case MessageState::Answered:
      return "answered";
    case MessageState::Expired:
      return "expired";
    case MessageState::Received:
      return "received";
  }
  return "sent";
}

inline std::optional<MessageState> messageStateFromString(std::string_view value)
{
  if (value == "sent")
    return MessageState::Sent;
  if (value == "delivered")
    return MessageState::Delivered;
  if (value == "notified")
    return MessageState::Notified;
  if (value == "answered")
    return MessageState::Answered;
  if (value == "expired")
    return MessageState::Expired;
  if (value == "received")
    return MessageState::Received;
  return std::nullopt;
}
