#pragma once

#include <cstdint>
#include <optional>
#include <string>

enum class DecisionMode : uint8_t
{
  Shadow = 0,
  Enforce
};

inline std::string decisionModeToString(DecisionMode mode)
{
  return mode == DecisionMode::Enforce ? "enforce" : "shadow";
}

inline std::optional<DecisionMode> decisionModeFromString(const std::string& value)
{
  if (value == "shadow")
    return DecisionMode::Shadow;
  if (value == "enforce")
    return DecisionMode::Enforce;
  return std::nullopt;
}
