#pragma once

#include <cstdint>
#include <optional>
#include <string>

enum class DecisionSuppression : uint8_t
{
  None = 0,
  BeliefGate,
  Budget,
  LegacySilent,
  ThreadSuppressed,
  Staging,
  Grouped,
  Held
};

inline std::string decisionSuppressionToString(DecisionSuppression reason)
{
  switch (reason) {
    case DecisionSuppression::None:
      return "none";
    case DecisionSuppression::BeliefGate:
      return "belief_gate";
    case DecisionSuppression::Budget:
      return "budget";
    case DecisionSuppression::LegacySilent:
      return "legacy_silent";
    case DecisionSuppression::ThreadSuppressed:
      return "thread_suppressed";
    case DecisionSuppression::Staging:
      return "staging";
    case DecisionSuppression::Grouped:
      return "grouped";
    case DecisionSuppression::Held:
      return "held";
  }
  return "none";
}

inline std::optional<DecisionSuppression>
decisionSuppressionFromString(const std::string& value)
{
  if (value == "none")
    return DecisionSuppression::None;
  if (value == "belief_gate")
    return DecisionSuppression::BeliefGate;
  if (value == "budget")
    return DecisionSuppression::Budget;
  if (value == "legacy_silent")
    return DecisionSuppression::LegacySilent;
  if (value == "thread_suppressed")
    return DecisionSuppression::ThreadSuppressed;
  if (value == "staging")
    return DecisionSuppression::Staging;
  if (value == "grouped")
    return DecisionSuppression::Grouped;
  if (value == "held")
    return DecisionSuppression::Held;
  return std::nullopt;
}
