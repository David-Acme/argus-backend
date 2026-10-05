#pragma once
#include "audit-log-query.hxx"

#include <drogon/utils/coroutine.h>
#include <optional>
#include <shared/schemas/audit-log/audit-log-schema.hxx>
#include <unordered_map>
#include <vector>

class AuditLogRepository
{
public:
  AuditLogRepository() = default;

  [[nodiscard]] drogon::Task<AuditLogSchema>
  create(const AuditLogCreateInput& input) const;
  [[nodiscard]] drogon::Task<std::optional<AuditLogSchema>>
  findExist(const AuditLogFindExistInput& input) const;
  [[nodiscard]] drogon::Task<void>
  remove(int64_t id, drogon::orm::DbClient* client = nullptr) const;

  [[nodiscard]] drogon::Task<std::vector<Json::Value>>
  findSync(const AuditLogSyncFilter& filter) const;
  [[nodiscard]] drogon::Task<std::optional<Json::Value>>
  findLastSync(const AuditLogSyncFilter& filter) const;

  [[nodiscard]] drogon::Task<std::vector<AuditLogCompactionPair>>
  findCompactionPairs(const AuditLogCompactionWindow& window) const;
  [[nodiscard]] drogon::Task<std::unordered_map<int64_t, Json::Value>>
  findCompactionChanges(const std::vector<int64_t>& ids) const;
  [[nodiscard]] drogon::Task<int64_t> findCompactionFrontier() const;
  [[nodiscard]] drogon::Task<void>
  compactRow(const AuditLogCompactInput& input) const;
  [[nodiscard]] drogon::Task<void>
  removeMany(const std::vector<int64_t>& ids,
             drogon::orm::DbClient* client = nullptr) const;
  [[nodiscard]] drogon::Task<void>
  advanceCompactionFrontier(int64_t throughId,
                            drogon::orm::DbClient* client = nullptr) const;
};
