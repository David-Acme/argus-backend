#include "user-audit-log-repository.hxx"

#include <config/app-config.hxx>
#include <ctime>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/utils/json-util/json-util.hxx>

using namespace user_audit_log_query;

drogon::Task<UserAuditLogSchema>
UserAuditLogRepository::create(const UserAuditLogCreateInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      INSERT.data(), input.userId, input.recordId,
      tableNameToString(input.tableName), json_util::toString(input.changes),
      static_cast<int>(input.priority), input.eventTimestamp);

  UserAuditLogSchema schema;
  schema.id = result.insertId();
  schema.userId = input.userId;
  schema.recordId = input.recordId;
  schema.tableName = input.tableName;
  schema.changes = input.changes;
  schema.priority = input.priority;
  schema.eventTimestamp = input.eventTimestamp;
  schema.createdAt = std::time(nullptr);
  co_return schema;
}

drogon::Task<std::optional<UserAuditLogSchema>>
UserAuditLogRepository::findExist(const UserAuditLogFindExistInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      FIND_EXIST.data(), input.userId, input.recordId,
      tableNameToString(input.tableName), input.dayStart, input.dayEnd);
  if (result.empty())
    co_return std::nullopt;
  co_return UserAuditLogSchema(result.front());
}

drogon::Task<void> UserAuditLogRepository::updateChanges(
    const UserAuditLogUpdateInput& input) const
{
  auto client = DbService::client();
  co_await client->execSqlCoro(UPDATE_CHANGES.data(),
                               json_util::toString(input.changes),
                               input.eventTimestamp, input.id);
}

drogon::Task<void> UserAuditLogRepository::remove(int64_t id) const
{
  auto client = DbService::client();
  co_await client->execSqlCoro(REMOVE.data(), id);
}

drogon::Task<std::vector<Json::Value>>
UserAuditLogRepository::findSync(const UserAuditLogSyncFilter& filter) const
{
  auto client = DbService::client();
  if (filter.startTime && filter.endTime) {
    const auto result =
        co_await client->execSqlCoro(std::string(FIND_SYNC) + AppConfig::SYNC_LIMIT, filter.userId,
                                     *filter.startTime, *filter.endTime);
    std::vector<Json::Value> data;
    for (const auto& row : result)
      data.push_back(UserAuditLogSchema(row).toJson());
    co_return data;
  }
  if (filter.startTime) {
    const auto result =
        co_await client->execSqlCoro(std::string(FIND_SYNC_FROM) + AppConfig::SYNC_LIMIT, filter.userId,
                                     *filter.startTime);
    std::vector<Json::Value> data;
    for (const auto& row : result)
      data.push_back(UserAuditLogSchema(row).toJson());
    co_return data;
  }
  if (filter.endTime) {
    const auto result = co_await client->execSqlCoro(
        std::string(FIND_SYNC_TO) + AppConfig::SYNC_LIMIT, filter.userId, *filter.endTime);
    std::vector<Json::Value> data;
    for (const auto& row : result)
      data.push_back(UserAuditLogSchema(row).toJson());
    co_return data;
  }
  {
    const auto result = co_await client->execSqlCoro(std::string(FIND_SYNC_ALL) + AppConfig::SYNC_LIMIT,
                                                     filter.userId);
    std::vector<Json::Value> data;
    for (const auto& row : result)
      data.push_back(UserAuditLogSchema(row).toJson());
    co_return data;
  }
}

drogon::Task<std::optional<Json::Value>>
UserAuditLogRepository::findLastSync(const UserAuditLogSyncFilter& filter) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(FIND_LAST_SYNC.data(), filter.userId);
  if (result.empty())
    co_return std::nullopt;
  co_return UserAuditLogSchema(result.front()).toJson();
}
