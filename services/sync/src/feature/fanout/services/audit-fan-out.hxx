#pragma once

#include <json/value.h>
#include <shared/repositories/user-action-log/user-action-log-repository.hxx>
#include <shared/services/audit-log/audit-log-service.hxx>
#include <shared/services/user-audit-log/user-audit-log-service.hxx>
#include <sync/module-audit-event.hxx>
#include <sync/user-action-event.hxx>
#include <sync/user-audit-event.hxx>

// The change feed's audit leg: the row is persisted first and the DB-assigned
// row is what the fan-out carries (Ruling Y). A frame naming recipients writes
// one user_audit_log row per recipient; one that does not writes a module
// audit_log row. The journal is append-only and reaches a client through its
// own Synchronize page, never through a live frame.
class AuditFanOut
{
public:
  void handleAuditChange(const Json::Value& json);
  void handleActionJournal(const Json::Value& json);

private:
  void insertModuleAudit(const ModuleAuditEvent& event);
  void insertUsersAudit(const UserAuditEvent& event);
  void insertAction(const UserActionEvent& event);

  AuditLogService auditLogService_;
  UserAuditLogService userAuditLogService_;
  UserActionLogRepository userActionLogRepository_;
};
