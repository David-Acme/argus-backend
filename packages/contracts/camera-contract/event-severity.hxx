#pragma once

#include <cstdint>
#include <string>

enum class EventSeverity : uint8_t
{
  Info = 0,
  Warning,
  Critical
};

inline std::string eventSeverityToString(EventSeverity s)
{
  switch (s) {
    case EventSeverity::Info:
      return "info";
    case EventSeverity::Warning:
      return "warning";
    case EventSeverity::Critical:
      return "critical";
  }
  return "info";
}

inline EventSeverity eventSeverityFromString(const std::string& s)
{
  if (s == "warning")
    return EventSeverity::Warning;
  if (s == "critical")
    return EventSeverity::Critical;
  return EventSeverity::Info;
}
