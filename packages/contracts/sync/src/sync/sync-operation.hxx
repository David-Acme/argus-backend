#pragma once

#include <cstdint>
#include <string>

enum class SyncOperation : uint8_t
{
  InitialInfo = 0,
  Synchronize = 1,
  SynchronizeAuditLog = 2,
  SynchronizeUserAuditLog = 3,
  Add = 4,
  Delete = 5,
  Log = 6,
  AuthContextChanged = 7,
  CallIncoming = 8,
  CallCancel = 9
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
    case SyncOperation::CallIncoming:
      return "call_incoming";
    case SyncOperation::CallCancel:
      return "call_cancel";
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
  if (s == "call_incoming")
    return SyncOperation::CallIncoming;
  if (s == "call_cancel")
    return SyncOperation::CallCancel;
  return SyncOperation::Synchronize;
}
