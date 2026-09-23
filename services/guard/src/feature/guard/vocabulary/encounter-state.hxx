#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

enum class EncounterState : uint8_t
{
  Observing = 0,
  Verifying,
  Challenging,
  Listening,
  Interpreting,
  Resolved,
  Escalating,
  Degraded,
  Closed
};

inline std::string encounterStateToString(EncounterState state)
{
  switch (state) {
    case EncounterState::Observing:
      return "observing";
    case EncounterState::Verifying:
      return "verifying";
    case EncounterState::Challenging:
      return "challenging";
    case EncounterState::Listening:
      return "listening";
    case EncounterState::Interpreting:
      return "interpreting";
    case EncounterState::Resolved:
      return "resolved";
    case EncounterState::Escalating:
      return "escalating";
    case EncounterState::Degraded:
      return "degraded";
    case EncounterState::Closed:
      return "closed";
  }
  return "closed";
}

inline EncounterState encounterStateFromString(const std::string& value)
{
  static const std::unordered_map<std::string, EncounterState> kMap = {
      {"observing", EncounterState::Observing},
      {"verifying", EncounterState::Verifying},
      {"challenging", EncounterState::Challenging},
      {"listening", EncounterState::Listening},
      {"interpreting", EncounterState::Interpreting},
      {"resolved", EncounterState::Resolved},
      {"escalating", EncounterState::Escalating},
      {"degraded", EncounterState::Degraded},
      {"closed", EncounterState::Closed},
  };
  const auto found = kMap.find(value);
  return found == kMap.end() ? EncounterState::Closed : found->second;
}
