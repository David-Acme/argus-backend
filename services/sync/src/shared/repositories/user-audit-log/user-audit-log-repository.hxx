#pragma once
#include "user-audit-log-query.hxx"

#include <drogon/utils/coroutine.h>
#include <optional>
#include <shared/schemas/user-audit-log/user-audit-log-schema.hxx>
#include <unordered_map>
#include <vector>

class UserAuditLogRepository
{
public:
  UserAuditLogRepository() = default;

  [[nodiscard]] drogon::Task<UserAuditLogSchema>
  create(const UserAuditLogCreateInput& input) const;
  [[nodiscard]] drogon::Task<std::optional<UserAuditLogSchema>>
  findExist(const UserAuditLogFindExistInput& input) const;
  [[nodiscard]] drogon::Task<void>
  remove(int64_t id, drogon::orm::DbClient* client = nullptr) const;

  [[nodiscard]] drogon::Task<std::vector<Json::Value>>
  findSync(const UserAuditLogSyncFilter& filter) const;
  [[nodiscard]] drogon::Task<std::optional<Json::Value>>
  findLastSync(const UserAuditLogSyncFilter& filter) const;

  [[nodiscard]] drogon::Task<std::vector<UserAuditLogCompactionPair>>
  findCompactionPairs(int64_t cutoffMs) const;
  [[nodiscard]] drogon::Task<std::unordered_map<int64_t, Json::Value>>
  findCompactionChanges(const std::vector<int64_t>& ids) const;
  [[nodiscard]] drogon::Task<int64_t> findCompactionFrontier() const;
  [[nodiscard]] drogon::Task<void>
  compactRow(const UserAuditLogCompactInput& input) const;
  [[nodiscard]] drogon::Task<void>
  removeMany(const std::vector<int64_t>& ids,
             drogon::orm::DbClient* client = nullptr) const;
  [[nodiscard]] drogon::Task<void>
  advanceCompactionFrontier(int64_t throughId,
                            drogon::orm::DbClient* client = nullptr) const;
};
