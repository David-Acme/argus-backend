#pragma once
#include "audit-log-query.hxx"

#include <drogon/utils/coroutine.h>
#include <shared/schemas/audit-log/audit-log-schema.hxx>

class AuditLogRepository
{
public:
  AuditLogRepository() = default;

  drogon::Task<std::vector<AuditLogSchema>>
  findByRecord(int64_t recordId, const std::string& tableName,
               int32_t limit = 50) const;
  drogon::Task<std::vector<AuditLogSchema>>
  findByTable(const std::string& tableName, int32_t limit = 50) const;
};
