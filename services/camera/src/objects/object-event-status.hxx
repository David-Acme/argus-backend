#pragma once

#include <cstdint>
#include <string>

// Camera observation outbox lifecycle; the CHECK constraint mirrors it.
enum class ObjectEventStatus : uint8_t
{
  Pending = 0,
  Sent,
  OverflowDropped
};

inline std::string objectEventStatusToString(ObjectEventStatus status)
{
  switch (status) {
    case ObjectEventStatus::Pending:
      return "pending";
    case ObjectEventStatus::Sent:
      return "sent";
    case ObjectEventStatus::OverflowDropped:
      return "overflow_dropped";
  }
  return "pending";
}

inline ObjectEventStatus objectEventStatusFromString(const std::string& value)
{
  if (value == "sent")
    return ObjectEventStatus::Sent;
  if (value == "overflow_dropped")
    return ObjectEventStatus::OverflowDropped;
  return ObjectEventStatus::Pending;
}
