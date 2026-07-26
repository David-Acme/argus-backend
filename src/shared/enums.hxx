#pragma once

#include <cstdint>
#include <string>

enum class UserRole : uint8_t
{
  Owner = 0,
  Resident,
  Guard,
  Guest
};

inline std::string userRoleToString(UserRole r)
{
  switch (r) {
  case UserRole::Owner:
    return "owner";
  case UserRole::Resident:
    return "resident";
  case UserRole::Guard:
    return "guard";
  case UserRole::Guest:
    return "guest";
  }
  return "guest";
}

inline UserRole userRoleFromString(const std::string& s)
{
  if (s == "owner") return UserRole::Owner;
  if (s == "resident") return UserRole::Resident;
  if (s == "guard") return UserRole::Guard;
  return UserRole::Guest;
}

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
  if (s == "warning") return EventSeverity::Warning;
  if (s == "critical") return EventSeverity::Critical;
  return EventSeverity::Info;
}

enum class CameraRecordMode : uint8_t
{
  Events = 0,
  Continuous
};

inline std::string cameraRecordModeToString(CameraRecordMode m)
{
  return m == CameraRecordMode::Continuous ? "continuous" : "events";
}

inline CameraRecordMode cameraRecordModeFromString(const std::string& s)
{
  if (s == "continuous") return CameraRecordMode::Continuous;
  return CameraRecordMode::Events;
}

enum class ZoneType : uint8_t
{
  Monitor = 0,
  Alert,
  Exclude
};

inline std::string zoneTypeToString(ZoneType t)
{
  switch (t) {
  case ZoneType::Alert:
    return "alert";
  case ZoneType::Exclude:
    return "exclude";
  default:
    return "monitor";
  }
}

inline ZoneType zoneTypeFromString(const std::string& s)
{
  if (s == "alert") return ZoneType::Alert;
  if (s == "exclude") return ZoneType::Exclude;
  return ZoneType::Monitor;
}

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

inline ReminderDetailStatus
reminderDetailStatusFromString(const std::string& s)
{
  if (s == "in_progress") return ReminderDetailStatus::InProgress;
  if (s == "done") return ReminderDetailStatus::Done;
  if (s == "blocked") return ReminderDetailStatus::Blocked;
  return ReminderDetailStatus::Pending;
}

enum class UserAction : uint8_t
{
  Create = 0,
  Update,
  Delete
};

inline std::string userActionToString(UserAction a)
{
  switch (a) {
  case UserAction::Update:
    return "update";
  case UserAction::Delete:
    return "delete";
  default:
    return "create";
  }
}

inline UserAction userActionFromString(const std::string& s)
{
  if (s == "update") return UserAction::Update;
  if (s == "delete") return UserAction::Delete;
  return UserAction::Create;
}
