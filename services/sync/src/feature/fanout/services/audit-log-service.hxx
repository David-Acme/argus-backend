#pragma once

#include <drogon/utils/coroutine.h>
#include <shared/repositories/audit-log/audit-log-query.hxx>
#include <shared/repositories/audit-log/audit-log-repository.hxx>
#include <shared/schemas/audit-log/audit-log-schema.hxx>

class AuditLogService
{
public:
  AuditLogService() = default;

  [[nodiscard]] drogon::Task<AuditLogSchema>
  create(const AuditLogWriteInput& input) const;

  [[nodiscard]] drogon::Task<int64_t> compact(int64_t cutoffMs) const;

private:
  AuditLogRepository repository_;
};
