#pragma once

#include <cstdint>
#include <string>

enum class ReminderDetailStatus : uint8_t
{
  Pending = 0,
  InProgress,
  Done,
  Blocked
};

inline std::string reminderDetailStatusToString(ReminderDetailStatus s)
{
  switch (s) {
    case ReminderDetailStatus::InProgress:
      return "in_progress";
    case ReminderDetailStatus::Done:
      return "done";
    case ReminderDetailStatus::Blocked:
      return "blocked";
    default:
      return "pending";
  }
}

inline ReminderDetailStatus reminderDetailStatusFromString(const std::string& s)
{
  if (s == "in_progress")
    return ReminderDetailStatus::InProgress;
  if (s == "done")
    return ReminderDetailStatus::Done;
  if (s == "blocked")
    return ReminderDetailStatus::Blocked;
  return ReminderDetailStatus::Pending;
}
