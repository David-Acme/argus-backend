#pragma once

#include <drogon/utils/coroutine.h>
#include <feature/fanout/services/audit-log-service.hxx>
#include <shared/repositories/user-audit-log/user-audit-log-query.hxx>
#include <shared/repositories/user-audit-log/user-audit-log-repository.hxx>
#include <shared/schemas/user-audit-log/user-audit-log-schema.hxx>
#include <vector>

class UserAuditLogService
{
public:
  UserAuditLogService() = default;

  [[nodiscard]] drogon::Task<std::vector<UserAuditLogSchema>>
  createMany(const UserAuditLogBatchWriteInput& input) const;

  [[nodiscard]] drogon::Task<AuditCompactionRound>
  compact(const AuditCompactionStep& step) const;

private:
  UserAuditLogRepository repository_;
};
