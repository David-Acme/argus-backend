#pragma once

#include <feature/fanout/services/audit-log-service.hxx>
#include <feature/fanout/services/user-audit-log-service.hxx>
#include <json/value.h>
#include <shared/repositories/user-action-log/user-action-log-repository.hxx>
#include <sync/module-audit-event.hxx>
#include <sync/user-action-event.hxx>
#include <sync/user-audit-event.hxx>

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
