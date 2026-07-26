#include "audit-log-repository.hxx"

#include <shared/services/sqlite/db-service.hxx>
using namespace audit_log_query;

drogon::Task<std::vector<AuditLogSchema>>
AuditLogRepository::findByRecord(int64_t recordId, const std::string& tableName,
                                 int32_t limit) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_BY_RECORD.data(),
                                                   recordId, tableName, limit);
  std::vector<AuditLogSchema> logs;
  for (const auto& row : result)
    logs.push_back(AuditLogSchema(row));
  co_return logs;
}

drogon::Task<std::vector<AuditLogSchema>>
AuditLogRepository::findByTable(const std::string& tableName,
                                int32_t limit) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(FIND_BY_TABLE.data(), tableName, limit);
  std::vector<AuditLogSchema> logs;
  for (const auto& row : result)
    logs.push_back(AuditLogSchema(row));
  co_return logs;
}
