#pragma once

#include <drogon/utils/coroutine.h>
#include <shared/repositories/user-audit-log/user-audit-log-query.hxx>
#include <shared/repositories/user-audit-log/user-audit-log-repository.hxx>
#include <shared/schemas/user-audit-log/user-audit-log-schema.hxx>

class UserAuditLogService
{
public:
  UserAuditLogService() = default;

  [[nodiscard]] drogon::Task<UserAuditLogSchema>
  create(const UserAuditLogWriteInput& input) const;

  [[nodiscard]] drogon::Task<int64_t> compact(int64_t cutoffMs) const;

private:
  UserAuditLogRepository repository_;
};
