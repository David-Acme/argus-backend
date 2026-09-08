#pragma once
#include "audit-log-query.hxx"

#include <drogon/utils/coroutine.h>
#include <optional>
#include <shared/schemas/audit-log/audit-log-schema.hxx>

class AuditLogRepository
{
public:
  AuditLogRepository() = default;

  drogon::Task<AuditLogSchema> create(const AuditLogCreateInput& input) const;
  drogon::Task<std::optional<AuditLogSchema>>
  findExist(const AuditLogFindExistInput& input) const;
  drogon::Task<void> updateChanges(const AuditLogUpdateInput& input) const;
  drogon::Task<void> remove(int64_t id) const;

  drogon::Task<std::vector<Json::Value>>
  findSync(const AuditLogSyncFilter& filter) const;
  drogon::Task<std::optional<Json::Value>>
  findLastSync(const AuditLogSyncFilter& filter) const;
};
