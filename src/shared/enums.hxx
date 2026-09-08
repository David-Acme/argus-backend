#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

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
  if (s == "owner")
    return UserRole::Owner;
  if (s == "resident")
    return UserRole::Resident;
  if (s == "guard")
    return UserRole::Guard;
  return UserRole::Guest;
}

// Private objects stay out of the sync stream; the category drives retention and access policy.
enum class StoredFileCategory : uint8_t
{
  Portrait = 0,
  Attachment
};

inline std::string storedFileCategoryToString(StoredFileCategory category)
{
  return category == StoredFileCategory::Attachment ? "attachment" : "portrait";
}

inline StoredFileCategory storedFileCategoryFromString(const std::string& value)
{
  return value == "attachment" ? StoredFileCategory::Attachment
                               : StoredFileCategory::Portrait;
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
  if (s == "warning")
    return EventSeverity::Warning;
  if (s == "critical")
    return EventSeverity::Critical;
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
  if (s == "continuous")
    return CameraRecordMode::Continuous;
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
  if (s == "alert")
    return ZoneType::Alert;
  if (s == "exclude")
    return ZoneType::Exclude;
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

enum class UserAction : uint8_t
{
  Create = 0,
  Read,
  Update,
  Delete
};

inline std::string userActionToString(UserAction a)
{
  switch (a) {
    case UserAction::Read:
      return "read";
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
  if (s == "read")
    return UserAction::Read;
  if (s == "update")
    return UserAction::Update;
  if (s == "delete")
    return UserAction::Delete;
  return UserAction::Create;
}

enum class MemoryScope : uint8_t
{
  Global = 0,
  User,
  Person,
  Device,
  Role
};

inline std::string memoryScopeToString(MemoryScope s)
{
  switch (s) {
    case MemoryScope::User:
      return "user";
    case MemoryScope::Person:
      return "person";
    case MemoryScope::Device:
      return "device";
    case MemoryScope::Role:
      return "role";
    default:
      return "global";
  }
}

inline MemoryScope memoryScopeFromString(const std::string& s)
{
  if (s == "user")
    return MemoryScope::User;
  if (s == "person")
    return MemoryScope::Person;
  if (s == "device")
    return MemoryScope::Device;
  if (s == "role")
    return MemoryScope::Role;
  return MemoryScope::Global;
}

enum class MemoryType : uint8_t
{
  Persona = 0,
  Episodic,
  Instruction,
  System
};

inline std::string memoryTypeToString(MemoryType t)
{
  switch (t) {
    case MemoryType::Episodic:
      return "episodic";
    case MemoryType::Instruction:
      return "instruction";
    case MemoryType::System:
      return "system";
    default:
      return "persona";
  }
}

inline MemoryType memoryTypeFromString(const std::string& s)
{
  if (s == "episodic")
    return MemoryType::Episodic;
  if (s == "instruction")
    return MemoryType::Instruction;
  if (s == "system")
    return MemoryType::System;
  return MemoryType::Persona;
}

enum class PhraseKind : uint8_t
{
  Trigger = 0,
  Confirmation,
  StatementStart,
  RecallMarker,
  Interrogative,
  Filler,
  Cancellation
};

inline std::string phraseKindToString(PhraseKind k)
{
  switch (k) {
    case PhraseKind::Confirmation:
      return "confirmation";
    case PhraseKind::StatementStart:
      return "statement_start";
    case PhraseKind::RecallMarker:
      return "recall_marker";
    case PhraseKind::Interrogative:
      return "interrogative";
    case PhraseKind::Filler:
      return "filler";
    case PhraseKind::Cancellation:
      return "cancellation";
    default:
      return "trigger";
  }
}

inline PhraseKind phraseKindFromString(const std::string& s)
{
  if (s == "confirmation")
    return PhraseKind::Confirmation;
  if (s == "statement_start")
    return PhraseKind::StatementStart;
  if (s == "recall_marker")
    return PhraseKind::RecallMarker;
  if (s == "interrogative")
    return PhraseKind::Interrogative;
  if (s == "filler")
    return PhraseKind::Filler;
  if (s == "cancellation")
    return PhraseKind::Cancellation;
  return PhraseKind::Trigger;
}

enum class LexiconKind : uint8_t
{
  Predicate = 0,
  Kinship,
  FirstPerson,
  Stopword
};

inline std::string lexiconKindToString(LexiconKind k)
{
  switch (k) {
    case LexiconKind::Kinship:
      return "kinship";
    case LexiconKind::FirstPerson:
      return "first_person";
    case LexiconKind::Stopword:
      return "stopword";
    default:
      return "predicate";
  }
}

inline LexiconKind lexiconKindFromString(const std::string& s)
{
  if (s == "kinship")
    return LexiconKind::Kinship;
  if (s == "first_person")
    return LexiconKind::FirstPerson;
  if (s == "stopword")
    return LexiconKind::Stopword;
  return LexiconKind::Predicate;
}

enum class MemorySource : uint8_t
{
  Rule = 0,
  Llm,
  Ingest
};

inline std::string memorySourceToString(MemorySource s)
{
  switch (s) {
    case MemorySource::Llm:
      return "llm";
    case MemorySource::Ingest:
      return "ingest";
    default:
      return "rule";
  }
}

inline MemorySource memorySourceFromString(const std::string& s)
{
  if (s == "llm")
    return MemorySource::Llm;
  if (s == "ingest")
    return MemorySource::Ingest;
  return MemorySource::Rule;
}

enum class AuditLogPriority : uint8_t
{
  Low = 0,
  Medium = 1,
  High = 2
};

enum class TableName : uint8_t
{
  User = 0,
  UserInvitation,
  Person,
  PersonEvent,
  Event,
  Reminder,
  ReminderDetail,
  CalendarEvent,
  CalendarEventShare,
  Project,
  ProjectMember,
  ProjectTask,
  ContextNote,
  Camera,
  CameraStream,
  Zone,
  AuditLog,
  UserAuditLog,
  Notification,
  NotificationToken,
  UserActionLog,
  RefreshToken,
  FaceEmbedding,
  Memory
};

// Last TableName value; role sets sweeping the enum cannot miss a new table.
inline constexpr TableName kLastTableName = TableName::Memory;

inline std::string tableNameToString(TableName t)
{
  switch (t) {
    case TableName::User:
      return "user";
    case TableName::UserInvitation:
      return "user_invitation";
    case TableName::Person:
      return "person";
    case TableName::PersonEvent:
      return "person_event";
    case TableName::Event:
      return "event";
    case TableName::Reminder:
      return "reminder";
    case TableName::ReminderDetail:
      return "reminder_detail";
    case TableName::CalendarEvent:
      return "calendar_event";
    case TableName::CalendarEventShare:
      return "calendar_event_share";
    case TableName::Project:
      return "project";
    case TableName::ProjectMember:
      return "project_member";
    case TableName::ProjectTask:
      return "project_task";
    case TableName::ContextNote:
      return "context_note";
    case TableName::Camera:
      return "camera";
    case TableName::CameraStream:
      return "camera_stream";
    case TableName::Zone:
      return "zone";
    case TableName::AuditLog:
      return "audit_log";
    case TableName::UserAuditLog:
      return "user_audit_log";
    case TableName::Notification:
      return "notification";
    case TableName::NotificationToken:
      return "notification_token";
    case TableName::UserActionLog:
      return "user_action_log";
    case TableName::RefreshToken:
      return "refresh_token";
    case TableName::FaceEmbedding:
      return "face_embedding";
    case TableName::Memory:
      return "memory";
  }
  return "user";
}

inline TableName tableNameFromString(const std::string& s)
{
  static const std::unordered_map<std::string, TableName> kMap = {
      {"user", TableName::User},
      {"user_invitation", TableName::UserInvitation},
      {"person", TableName::Person},
      {"person_event", TableName::PersonEvent},
      {"event", TableName::Event},
      {"reminder", TableName::Reminder},
      {"reminder_detail", TableName::ReminderDetail},
      {"calendar_event", TableName::CalendarEvent},
      {"calendar_event_share", TableName::CalendarEventShare},
      {"project", TableName::Project},
      {"project_member", TableName::ProjectMember},
      {"project_task", TableName::ProjectTask},
      {"context_note", TableName::ContextNote},
      {"camera", TableName::Camera},
      {"camera_stream", TableName::CameraStream},
      {"zone", TableName::Zone},
      {"audit_log", TableName::AuditLog},
      {"user_audit_log", TableName::UserAuditLog},
      {"notification", TableName::Notification},
      {"notification_token", TableName::NotificationToken},
      {"user_action_log", TableName::UserActionLog},
      {"refresh_token", TableName::RefreshToken},
      {"face_embedding", TableName::FaceEmbedding},
      {"memory", TableName::Memory},
  };
  const auto it = kMap.find(s);
  if (it == kMap.end())
    return TableName::User;
  return it->second;
}

// Which integration drives a camera; the control layer picks a driver from it.
enum class CameraDriver : uint8_t
{
  Tapo = 0,
  Onvif,
  Rtsp
};

inline std::string cameraDriverToString(CameraDriver d)
{
  switch (d) {
    case CameraDriver::Onvif:
      return "onvif";
    case CameraDriver::Rtsp:
      return "rtsp";
    case CameraDriver::Tapo:
      return "tapo";
  }
  return "tapo";
}

inline CameraDriver cameraDriverFromString(const std::string& s)
{
  if (s == "onvif")
    return CameraDriver::Onvif;
  if (s == "rtsp")
    return CameraDriver::Rtsp;
  return CameraDriver::Tapo;
}

// What a member may do with a record shared with them; `View` is the default.
enum class ShareAccess : uint8_t
{
  View = 0,
  Edit
};

inline std::string shareAccessToString(ShareAccess a)
{
  return a == ShareAccess::Edit ? "edit" : "view";
}

inline ShareAccess shareAccessFromString(const std::string& s)
{
  return s == "edit" ? ShareAccess::Edit : ShareAccess::View;
}

// Why a share could not be granted; the controller maps each case to its own status.
enum class MembershipError : uint8_t
{
  None = 0,
  ParentNotFound,
  UserNotFound,
  UserNotAllowed,
  SelfShare
};

// Canonical voice interaction languages (STT + TTS + prompts); the DB stores the string code.
enum class VoiceLang : uint8_t
{
  System = 0,
  Es,
  En
};

inline std::string voiceLangToString(VoiceLang lang)
{
  switch (lang) {
    case VoiceLang::Es:
      return "es";
    case VoiceLang::En:
      return "en";
    case VoiceLang::System:
      return "";
  }
  return "";
}

inline VoiceLang voiceLangFromString(const std::string& s)
{
  static const std::unordered_map<std::string, VoiceLang> kMap = {
      {"es", VoiceLang::Es},
      {"en", VoiceLang::En},
  };
  const auto it = kMap.find(s);
  return it == kMap.end() ? VoiceLang::System : it->second;
}
