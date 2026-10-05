#pragma once

#include <drogon/utils/coroutine.h>
#include <shared/repositories/audit-log/audit-log-query.hxx>
#include <shared/repositories/audit-log/audit-log-repository.hxx>
#include <shared/schemas/audit-log/audit-log-schema.hxx>

struct AuditCompactionStep
{
  int64_t cutoffMs{0};
  int64_t afterId{0};
};

struct AuditCompactionRound
{
  int64_t removed{0};
  int64_t lastOlderId{0};
};

class AuditLogService
{
public:
  AuditLogService() = default;

  [[nodiscard]] drogon::Task<AuditLogSchema>
  create(const AuditLogWriteInput& input) const;

  [[nodiscard]] drogon::Task<AuditCompactionRound>
  compact(const AuditCompactionStep& step) const;

private:
  AuditLogRepository repository_;
};
