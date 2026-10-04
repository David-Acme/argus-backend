#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

enum class CallTrigger : uint8_t
{
  GuardCritical = 0,
  GuardIntruder,
  GuardEscalation,
  GuardArrival,
  Agenda,
  Assistant
};

inline constexpr std::array<CallTrigger, 6> kCallTriggers{
    CallTrigger::GuardCritical, CallTrigger::GuardIntruder,
    CallTrigger::GuardEscalation, CallTrigger::GuardArrival,
    CallTrigger::Agenda, CallTrigger::Assistant};

inline std::string callTriggerToString(CallTrigger trigger)
{
  switch (trigger) {
    case CallTrigger::GuardCritical:
      return "guard_critical";
    case CallTrigger::GuardIntruder:
      return "guard_intruder";
    case CallTrigger::GuardEscalation:
      return "guard_escalation";
    case CallTrigger::GuardArrival:
      return "guard_arrival";
    case CallTrigger::Agenda:
      return "agenda";
    case CallTrigger::Assistant:
      return "assistant";
  }
  return "assistant";
}

inline std::optional<CallTrigger> callTriggerFromString(std::string_view value)
{
  for (const CallTrigger trigger : kCallTriggers) {
    if (callTriggerToString(trigger) == value)
      return trigger;
  }
  return std::nullopt;
}

inline bool isGuardTrigger(CallTrigger trigger)
{
  return trigger == CallTrigger::GuardCritical ||
         trigger == CallTrigger::GuardIntruder ||
         trigger == CallTrigger::GuardEscalation ||
         trigger == CallTrigger::GuardArrival;
}

inline std::string_view callKindOf(CallTrigger trigger)
{
  switch (trigger) {
    case CallTrigger::GuardCritical:
    case CallTrigger::GuardIntruder:
    case CallTrigger::GuardEscalation:
      return "guard_episode";
    case CallTrigger::GuardArrival:
      return "guard_arrival";
    case CallTrigger::Agenda:
      return "agenda";
    case CallTrigger::Assistant:
      return "assistant";
  }
  return "assistant";
}
