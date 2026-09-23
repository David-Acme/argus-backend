#pragma once

#include <drogon/utils/coroutine.h>
#include <shared/repositories/audit-log/audit-log-query.hxx>
#include <shared/repositories/audit-log/audit-log-repository.hxx>
#include <shared/schemas/audit-log/audit-log-schema.hxx>

class AuditLogService
{
public:
  AuditLogService() = default;

  drogon::Task<AuditLogSchema> create(const AuditLogWriteInput& input) const;

private:
  AuditLogRepository repository_;
};
