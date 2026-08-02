#pragma once

#include <cstdint>
#include <string>

enum class SyncOperation : uint8_t
{
  InitialInfo = 0,             // info actual del usuario (al conectar)
  Synchronize = 1,             // sync de datos creados/eliminados (global + nivel usuario)
  SynchronizeAuditLog = 2,     // sync de actualizaciones atómicas GLOBALES
  SynchronizeUserAuditLog = 3, // sync de actualizaciones atómicas A NIVEL USUARIO
  Add = 4,                     // evento en vivo: entidad/notificación creada
  Delete = 5,                  // evento en vivo: entidad eliminada
  Log = 6                      // evento en vivo: audit log
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
  return SyncOperation::Synchronize;
}
