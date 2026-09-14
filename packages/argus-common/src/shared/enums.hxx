#pragma once

#include <cstdint>
#include <optional>
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

// Private objects stay out of the sync stream; the category drives retention
// and access policy.
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

// Why a share could not be granted; the controller maps each case to its own
// status.
enum class MembershipError : uint8_t
{
  None = 0,
  ParentNotFound,
  UserNotFound,
  UserNotAllowed,
  SelfShare
};

// How far a person row is trusted: auto-enrolled sightings are candidates only.
enum class PersonStatus : uint8_t
{
  Candidate = 0,
  Known
};

inline std::string personStatusToString(PersonStatus status)
{
  return status == PersonStatus::Known ? "known" : "candidate";
}

inline PersonStatus personStatusFromString(const std::string& value)
{
  return value == "known" ? PersonStatus::Known : PersonStatus::Candidate;
}

// Encounter lifecycle; every transition is a deterministic command, never model
// output.
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

// Site occupancy profile; only the matrix decides what each mode means.
enum class GuardMode : uint8_t
{
  Home = 0,
  Away,
  Night,
  Armed
};

inline std::string guardModeToString(GuardMode mode)
{
  switch (mode) {
    case GuardMode::Home:
      return "home";
    case GuardMode::Away:
      return "away";
    case GuardMode::Night:
      return "night";
    case GuardMode::Armed:
      return "armed";
  }
  return "home";
}

inline GuardMode guardModeFromString(const std::string& value)
{
  if (value == "away")
    return GuardMode::Away;
  if (value == "night")
    return GuardMode::Night;
  if (value == "armed")
    return GuardMode::Armed;
  return GuardMode::Home;
}

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

// Notification delivery intent lifecycle; the CHECK constraint mirrors it.
enum class NotificationDeliveryStatus : uint8_t
{
  Pending = 0,
  Sent
};

inline std::string
notificationDeliveryStatusToString(NotificationDeliveryStatus status)
{
  switch (status) {
    case NotificationDeliveryStatus::Pending:
      return "pending";
    case NotificationDeliveryStatus::Sent:
      return "sent";
  }
  return "pending";
}

inline NotificationDeliveryStatus
notificationDeliveryStatusFromString(const std::string& value)
{
  return value == "sent" ? NotificationDeliveryStatus::Sent
                         : NotificationDeliveryStatus::Pending;
}

// Gateway-side delivery receipt lifecycle; the CHECK constraint mirrors it.
// Unknown persisted values fail closed at the call site: fromString returns
// nullopt instead of defaulting to a re-executable state.
enum class NotificationDeliveryReceipt : uint8_t
{
  Received = 0,
  Dispatched,
  Conflict,
  DeadLettered
};

inline std::string
notificationDeliveryReceiptToString(NotificationDeliveryReceipt status)
{
  switch (status) {
    case NotificationDeliveryReceipt::Received:
      return "received";
    case NotificationDeliveryReceipt::Dispatched:
      return "dispatched";
    case NotificationDeliveryReceipt::Conflict:
      return "conflict";
    case NotificationDeliveryReceipt::DeadLettered:
      return "dead_lettered";
  }
  return "received";
}

inline std::optional<NotificationDeliveryReceipt>
notificationDeliveryReceiptFromString(const std::string& value)
{
  if (value == "received")
    return NotificationDeliveryReceipt::Received;
  if (value == "dispatched")
    return NotificationDeliveryReceipt::Dispatched;
  if (value == "conflict")
    return NotificationDeliveryReceipt::Conflict;
  if (value == "dead_lettered")
    return NotificationDeliveryReceipt::DeadLettered;
  return std::nullopt;
}

// LLM-side encounter receipt lifecycle; the CHECK constraint mirrors it.
// Unknown persisted values fail closed at the call site.
enum class EncounterClosedReceipt : uint8_t
{
  Received = 0,
  Dispatched,
  Conflict,
  DeadLettered
};

inline std::string
encounterClosedReceiptToString(EncounterClosedReceipt status)
{
  switch (status) {
    case EncounterClosedReceipt::Received:
      return "received";
    case EncounterClosedReceipt::Dispatched:
      return "dispatched";
    case EncounterClosedReceipt::Conflict:
      return "conflict";
    case EncounterClosedReceipt::DeadLettered:
      return "dead_lettered";
  }
  return "received";
}

inline std::optional<EncounterClosedReceipt>
encounterClosedReceiptFromString(const std::string& value)
{
  if (value == "received")
    return EncounterClosedReceipt::Received;
  if (value == "dispatched")
    return EncounterClosedReceipt::Dispatched;
  if (value == "conflict")
    return EncounterClosedReceipt::Conflict;
  if (value == "dead_lettered")
    return EncounterClosedReceipt::DeadLettered;
  return std::nullopt;
}

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

// Durable inbox lifecycle of one observation; the CHECK constraint mirrors it.
enum class ObservationStatus : uint8_t
{
  Processing = 0,
  Completed,
  DeadLettered
};

inline std::string observationStatusToString(ObservationStatus status)
{
  switch (status) {
    case ObservationStatus::Processing:
      return "processing";
    case ObservationStatus::Completed:
      return "completed";
    case ObservationStatus::DeadLettered:
      return "dead_lettered";
  }
  return "processing";
}

inline ObservationStatus observationStatusFromString(const std::string& value)
{
  if (value == "completed")
    return ObservationStatus::Completed;
  if (value == "dead_lettered")
    return ObservationStatus::DeadLettered;
  return ObservationStatus::Processing;
}

// Durable lifecycle of one guard action intent; resumable states are replayed
// under the same command id, terminal states carry their distinct meaning.
enum class GuardIntentStatus : uint8_t
{
  Pending = 0,
  InFlight,
  RetryableFailed,
  Succeeded,
  DuplicateSucceeded,
  Rejected,
  Conflict,
  Indeterminate
};

inline std::string guardIntentStatusToString(GuardIntentStatus status)
{
  switch (status) {
    case GuardIntentStatus::Pending:
      return "pending";
    case GuardIntentStatus::InFlight:
      return "in_flight";
    case GuardIntentStatus::RetryableFailed:
      return "retryable_failed";
    case GuardIntentStatus::Succeeded:
      return "succeeded";
    case GuardIntentStatus::DuplicateSucceeded:
      return "duplicate_succeeded";
    case GuardIntentStatus::Rejected:
      return "rejected";
    case GuardIntentStatus::Conflict:
      return "conflict";
    case GuardIntentStatus::Indeterminate:
      return "indeterminate";
  }
  return "pending";
}

inline std::optional<GuardIntentStatus>
guardIntentStatusFromString(const std::string& value)
{
  static const std::unordered_map<std::string, GuardIntentStatus> kMap = {
      {"pending", GuardIntentStatus::Pending},
      {"in_flight", GuardIntentStatus::InFlight},
      {"retryable_failed", GuardIntentStatus::RetryableFailed},
      {"succeeded", GuardIntentStatus::Succeeded},
      {"duplicate_succeeded", GuardIntentStatus::DuplicateSucceeded},
      {"rejected", GuardIntentStatus::Rejected},
      {"conflict", GuardIntentStatus::Conflict},
      {"indeterminate", GuardIntentStatus::Indeterminate},
  };
  const auto found = kMap.find(value);
  if (found == kMap.end())
    return std::nullopt;
  return found->second;
}

inline bool guardIntentStatusIsResumable(GuardIntentStatus status)
{
  return status == GuardIntentStatus::Pending ||
         status == GuardIntentStatus::InFlight ||
         status == GuardIntentStatus::RetryableFailed;
}

inline bool guardIntentStatusIsTerminal(GuardIntentStatus status)
{
  return !guardIntentStatusIsResumable(status);
}

// One physical or user-visible effect the guard may raise; the authorizer owns
// the mapping.
enum class GuardActionKind : uint8_t
{
  Greet = 0,
  Listen,
  Reply,
  Announce,
  Alarm,
  SirenArm,
  SirenDisarm,
  Notify
};

inline std::string guardActionKindToString(GuardActionKind kind)
{
  switch (kind) {
    case GuardActionKind::Greet:
      return "greet";
    case GuardActionKind::Listen:
      return "greet_listen";
    case GuardActionKind::Reply:
      return "greet_reply";
    case GuardActionKind::Announce:
      return "announce";
    case GuardActionKind::Alarm:
      return "alarm";
    case GuardActionKind::SirenArm:
      return "siren_arm";
    case GuardActionKind::SirenDisarm:
      return "siren_disarm";
    case GuardActionKind::Notify:
      return "notify";
  }
  return "notify";
}

inline GuardActionKind guardActionKindFromString(const std::string& value)
{
  static const std::unordered_map<std::string, GuardActionKind> kMap = {
      {"greet", GuardActionKind::Greet},
      {"greet_listen", GuardActionKind::Listen},
      {"greet_reply", GuardActionKind::Reply},
      {"announce", GuardActionKind::Announce},
      {"alarm", GuardActionKind::Alarm},
      {"siren_arm", GuardActionKind::SirenArm},
      {"siren_disarm", GuardActionKind::SirenDisarm},
      {"notify", GuardActionKind::Notify},
  };
  const auto found = kMap.find(value);
  return found == kMap.end() ? GuardActionKind::Notify : found->second;
}

// Canonical voice interaction languages (STT + TTS + prompts); the DB stores
// the string code.
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
