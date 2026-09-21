#pragma once

#include <cstdint>
#include <string>
#include <unordered_map>

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
