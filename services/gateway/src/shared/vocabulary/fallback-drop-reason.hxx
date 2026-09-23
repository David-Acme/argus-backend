#pragma once

#include <cstdint>
#include <optional>
#include <string>

enum class FallbackDropReason : uint8_t
{
  NonHardSignal = 0,
  DropKnown,
  DropWeakScore,
  DropShortDwell,
  BudgetSilent
};

inline std::string fallbackDropReasonToString(FallbackDropReason reason)
{
  switch (reason) {
    case FallbackDropReason::NonHardSignal:
      return "non_hard_signal";
    case FallbackDropReason::DropKnown:
      return "drop_known";
    case FallbackDropReason::DropWeakScore:
      return "drop_weak_score";
    case FallbackDropReason::DropShortDwell:
      return "drop_short_dwell";
    case FallbackDropReason::BudgetSilent:
      return "budget_silent";
  }
  return "non_hard_signal";
}

inline std::optional<FallbackDropReason>
fallbackDropReasonFromString(const std::string& value)
{
  if (value == "non_hard_signal")
    return FallbackDropReason::NonHardSignal;
  if (value == "drop_known")
    return FallbackDropReason::DropKnown;
  if (value == "drop_weak_score")
    return FallbackDropReason::DropWeakScore;
  if (value == "drop_short_dwell")
    return FallbackDropReason::DropShortDwell;
  if (value == "budget_silent")
    return FallbackDropReason::BudgetSilent;
  return std::nullopt;
}
