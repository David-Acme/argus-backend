#pragma once

#include <cstdint>
#include <string>

// Deterministic danger tiers; the model may inform but never assigns this
// value.
enum class GuardDanger : uint8_t
{
  None = 0,
  Low,
  Medium,
  High,
  Critical
};

inline std::string guardDangerToString(GuardDanger danger)
{
  switch (danger) {
    case GuardDanger::None:
      return "none";
    case GuardDanger::Low:
      return "low";
    case GuardDanger::Medium:
      return "medium";
    case GuardDanger::High:
      return "high";
    case GuardDanger::Critical:
      return "critical";
  }
  return "none";
}

inline GuardDanger guardDangerFromString(const std::string& value)
{
  if (value == "low")
    return GuardDanger::Low;
  if (value == "medium")
    return GuardDanger::Medium;
  if (value == "high")
    return GuardDanger::High;
  if (value == "critical")
    return GuardDanger::Critical;
  return GuardDanger::None;
}

inline int guardDangerRank(GuardDanger danger)
{
  return static_cast<int>(danger);
}
