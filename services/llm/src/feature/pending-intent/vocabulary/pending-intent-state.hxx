#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

enum class PendingIntentState : std::uint8_t
{
  Offered,
  Waiting,
  Done,
  Failed,
  Expired
};

constexpr std::string_view pendingIntentStateToString(PendingIntentState state)
{
  switch (state) {
  case PendingIntentState::Offered: return "offered";
  case PendingIntentState::Waiting: return "waiting";
  case PendingIntentState::Done: return "done";
  case PendingIntentState::Failed: return "failed";
  case PendingIntentState::Expired: return "expired";
  }
  return "expired";
}

constexpr std::optional<PendingIntentState> pendingIntentStateFromString(std::string_view text)
{
  if (text == "offered")
    return PendingIntentState::Offered;
  if (text == "waiting")
    return PendingIntentState::Waiting;
  if (text == "done")
    return PendingIntentState::Done;
  if (text == "failed")
    return PendingIntentState::Failed;
  if (text == "expired")
    return PendingIntentState::Expired;
  return std::nullopt;
}
