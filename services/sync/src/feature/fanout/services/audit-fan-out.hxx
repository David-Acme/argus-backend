#pragma once

#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/fanout/services/audit-log-service.hxx>
#include <feature/fanout/services/user-audit-log-service.hxx>
#include <json/value.h>
#include <shared/repositories/user-action-log/user-action-log-repository.hxx>
#include <string_view>
#include <sync/module-audit-event.hxx>
#include <sync/user-action-event.hxx>
#include <sync/user-audit-event.hxx>

enum class ActionJournalOutcome : std::uint8_t
{
  Acknowledged = 0,
  Duplicate,
  Refused
};

class AuditFanOut
{
public:
  [[nodiscard]] bool migrateLegacySchema() const;
  [[nodiscard]] bool backfillActivityModules() const;

  drogon::Task<bool> handleAuditChange(const Json::Value& json);
  drogon::Task<ActionJournalOutcome> handleActionJournal(const Json::Value& json,
                                                          std::string_view msgId);

private:
  drogon::Task<void> insertModuleAudit(const ModuleAuditEvent& event);
  drogon::Task<void> insertUsersAudit(const UserAuditEvent& event);
  drogon::Task<bool> insertAction(const UserActionEvent& event,
                                  std::string_view msgId);

  AuditLogService auditLogService_;
  UserAuditLogService userAuditLogService_;
  UserActionLogRepository userActionLogRepository_;
};
