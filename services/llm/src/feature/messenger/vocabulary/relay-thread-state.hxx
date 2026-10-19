#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

enum class RelayThreadState : std::uint8_t
{
  Open,
  Answered,
  Expired
};

constexpr std::string_view relayThreadStateToString(RelayThreadState state)
{
  switch (state) {
  case RelayThreadState::Open: return "open";
  case RelayThreadState::Answered: return "answered";
  case RelayThreadState::Expired: return "expired";
  }
  return {};
}

constexpr std::optional<RelayThreadState> relayThreadStateFromString(std::string_view text)
{
  if (text == "open")
    return RelayThreadState::Open;
  if (text == "answered")
    return RelayThreadState::Answered;
  if (text == "expired")
    return RelayThreadState::Expired;
  return std::nullopt;
}
