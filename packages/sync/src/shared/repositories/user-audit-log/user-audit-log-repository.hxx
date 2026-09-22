#pragma once
#include "user-audit-log-query.hxx"

#include <drogon/utils/coroutine.h>
#include <optional>
#include <shared/schemas/user-audit-log/user-audit-log-schema.hxx>

class UserAuditLogRepository
{
public:
  UserAuditLogRepository() = default;

  drogon::Task<UserAuditLogSchema>
  create(const UserAuditLogCreateInput& input) const;
  drogon::Task<std::optional<UserAuditLogSchema>>
  findExist(const UserAuditLogFindExistInput& input) const;
  drogon::Task<void> updateChanges(const UserAuditLogUpdateInput& input) const;
  drogon::Task<void> remove(int64_t id) const;

  drogon::Task<std::vector<Json::Value>>
  findSync(const UserAuditLogSyncFilter& filter) const;
  drogon::Task<std::optional<Json::Value>>
  findLastSync(const UserAuditLogSyncFilter& filter) const;
};
