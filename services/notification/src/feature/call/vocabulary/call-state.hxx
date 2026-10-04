#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

enum class CallState : uint8_t
{
  Ringing = 0,
  Queued,
  Answered,
  Completed,
  Missed,
  Declined,
  Injected
};

inline std::string callStateToString(CallState state)
{
  switch (state) {
    case CallState::Ringing:
      return "ringing";
    case CallState::Queued:
      return "queued";
    case CallState::Answered:
      return "answered";
    case CallState::Completed:
      return "completed";
    case CallState::Missed:
      return "missed";
    case CallState::Declined:
      return "declined";
    case CallState::Injected:
      return "injected";
  }
  return "missed";
}

inline std::optional<CallState> callStateFromString(std::string_view value)
{
  if (value == "ringing")
    return CallState::Ringing;
  if (value == "queued")
    return CallState::Queued;
  if (value == "answered")
    return CallState::Answered;
  if (value == "completed")
    return CallState::Completed;
  if (value == "missed")
    return CallState::Missed;
  if (value == "declined")
    return CallState::Declined;
  if (value == "injected")
    return CallState::Injected;
  return std::nullopt;
}
