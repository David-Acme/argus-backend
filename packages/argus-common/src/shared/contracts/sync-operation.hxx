#pragma once

#include <cstdint>
#include <string>

enum class SyncOperation : uint8_t
{
  InitialInfo = 0,         // current user info (on connect)
  Synchronize = 1,         // sync of created/deleted data (global + user level)
  SynchronizeAuditLog = 2, // sync of GLOBAL atomic updates
  SynchronizeUserAuditLog = 3, // sync of atomic updates AT USER LEVEL
  Add = 4,                     // live event: entity/notification created
  Delete = 5,                  // live event: entity deleted
  Log = 6,                     // live event: audit log
  AuthContextChanged = 7       // live event: role/active context must refresh
};

inline std::string syncOperationToString(SyncOperation op)
{
  switch (op) {
    case SyncOperation::InitialInfo:
      return "initial_info";
    case SyncOperation::Synchronize:
      return "sync";
    case SyncOperation::SynchronizeAuditLog:
      return "sync_audit_log";
    case SyncOperation::SynchronizeUserAuditLog:
      return "sync_user_audit_log";
    case SyncOperation::Add:
      return "add";
    case SyncOperation::Delete:
      return "delete";
    case SyncOperation::Log:
      return "log";
    case SyncOperation::AuthContextChanged:
      return "auth_context_changed";
  }
  return "sync";
}

inline SyncOperation syncOperationFromString(const std::string& s)
{
  if (s == "initial_info")
    return SyncOperation::InitialInfo;
  if (s == "sync_audit_log")
    return SyncOperation::SynchronizeAuditLog;
  if (s == "sync_user_audit_log")
    return SyncOperation::SynchronizeUserAuditLog;
  if (s == "add")
    return SyncOperation::Add;
  if (s == "delete")
    return SyncOperation::Delete;
  if (s == "log")
    return SyncOperation::Log;
  if (s == "auth_context_changed")
    return SyncOperation::AuthContextChanged;
  return SyncOperation::Synchronize;
}
