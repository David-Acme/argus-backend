#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

enum class RelayChannel : std::uint8_t
{
  Conversation,
  Notification,
  Camera
};

constexpr std::string_view relayChannelToString(RelayChannel channel)
{
  switch (channel) {
  case RelayChannel::Conversation: return "conversation";
  case RelayChannel::Notification: return "notification";
  case RelayChannel::Camera: return "camera";
  }
  return {};
}

constexpr std::optional<RelayChannel> relayChannelFromString(std::string_view text)
{
  if (text == "conversation")
    return RelayChannel::Conversation;
  if (text == "notification")
    return RelayChannel::Notification;
  if (text == "camera")
    return RelayChannel::Camera;
  return std::nullopt;
}

enum class RelayOutcome : std::uint8_t
{
  Delivered,
  Notified,
  NoAnswer,
  NoRecipient,
  Unavailable
};

constexpr std::string_view relayOutcomeToString(RelayOutcome outcome)
{
  switch (outcome) {
  case RelayOutcome::Delivered: return "delivered";
  case RelayOutcome::Notified: return "notified";
  case RelayOutcome::NoAnswer: return "no_answer";
  case RelayOutcome::NoRecipient: return "no_recipient";
  case RelayOutcome::Unavailable: return "unavailable";
  }
  return {};
}

constexpr std::optional<RelayOutcome> relayOutcomeFromString(std::string_view text)
{
  if (text == "delivered")
    return RelayOutcome::Delivered;
  if (text == "notified")
    return RelayOutcome::Notified;
  if (text == "no_answer")
    return RelayOutcome::NoAnswer;
  if (text == "no_recipient")
    return RelayOutcome::NoRecipient;
  if (text == "unavailable")
    return RelayOutcome::Unavailable;
  return std::nullopt;
}
