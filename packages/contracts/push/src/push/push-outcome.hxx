#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

enum class PushOutcome : uint8_t
{
  Delivered = 0,
  Unregistered = 1,
  InvalidToken = 2,
  Throttled = 3,
  TokenBound = 4,
  Failed = 5
};

inline std::string_view pushOutcomeToString(PushOutcome outcome) noexcept
{
  switch (outcome) {
    case PushOutcome::Delivered:
      return "delivered";
    case PushOutcome::Unregistered:
      return "unregistered";
    case PushOutcome::InvalidToken:
      return "invalid_token";
    case PushOutcome::Throttled:
      return "throttled";
    case PushOutcome::TokenBound:
      return "token_bound";
    case PushOutcome::Failed:
      return "failed";
  }
  return "";
}

inline std::optional<PushOutcome>
pushOutcomeFromString(std::string_view name) noexcept
{
  if (name == "delivered")
    return PushOutcome::Delivered;
  if (name == "unregistered")
    return PushOutcome::Unregistered;
  if (name == "invalid_token")
    return PushOutcome::InvalidToken;
  if (name == "throttled")
    return PushOutcome::Throttled;
  if (name == "token_bound")
    return PushOutcome::TokenBound;
  if (name == "failed")
    return PushOutcome::Failed;
  return std::nullopt;
}
