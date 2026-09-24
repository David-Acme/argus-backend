#include "user-audit-log-repository.hxx"

#include <ctime>
#include <sync/sync-limits.hxx>
#include <sqlite/db-service.hxx>
#include <text/json-util.hxx>

using namespace user_audit_log_query;

namespace
{
std::string buildInPlaceholders(size_t count)
{
  std::string out;
  for (size_t i = 0; i < count; ++i) {
    if (i > 0)
      out += ", ";
    out += '?';
  }
  return out;
}

std::string expand(const std::string_view query,
                   const std::string& placeholders)
{
  std::string out(query);
  const std::string marker = "%1%";
  const auto pos = out.find(marker);
  if (pos != std::string::npos)
    out.replace(pos, marker.size(), placeholders);
  return out;
}
}

drogon::Task<UserAuditLogSchema>
UserAuditLogRepository::create(const UserAuditLogCreateInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      std::string(INSERT), input.userId, input.recordId,
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
      std::string(FIND_EXIST), input.userId, input.recordId,
      tableNameToString(input.tableName), input.dayStart, input.dayEnd);
  if (result.empty())
    co_return std::nullopt;
  co_return UserAuditLogSchema(result.front());
}

drogon::Task<void> UserAuditLogRepository::remove(int64_t id) const
{
  auto client = DbService::client();
  co_await client->execSqlCoro(std::string(REMOVE), id);
}

drogon::Task<std::vector<Json::Value>>
UserAuditLogRepository::findSync(const UserAuditLogSyncFilter& filter) const
{
  auto client = DbService::client();
  if (filter.afterId) {
    std::vector<Json::Value> data;
    if (filter.endId) {
      const auto result = co_await client->execSqlCoro(
          std::string(FIND_SYNC_AFTER_ID_TO) + SyncLimits::kMaxRows,
          filter.userId, *filter.afterId, *filter.endId);
      for (const auto& row : result)
        data.push_back(UserAuditLogSchema(row).toJson());
      co_return data;
    }
    const auto result = co_await client->execSqlCoro(
        std::string(FIND_SYNC_AFTER_ID) + SyncLimits::kMaxRows,
        filter.userId, *filter.afterId);
    for (const auto& row : result)
      data.push_back(UserAuditLogSchema(row).toJson());
    co_return data;
  }
  if (filter.startTime && filter.endTime) {
    const auto result =
        co_await client->execSqlCoro(std::string(FIND_SYNC) + SyncLimits::kMaxRows, filter.userId,
                                     *filter.startTime, *filter.endTime);
    std::vector<Json::Value> data;
    for (const auto& row : result)
      data.push_back(UserAuditLogSchema(row).toJson());
    co_return data;
  }
  if (filter.startTime) {
    const auto result =
        co_await client->execSqlCoro(std::string(FIND_SYNC_FROM) + SyncLimits::kMaxRows, filter.userId,
                                     *filter.startTime);
    std::vector<Json::Value> data;
    for (const auto& row : result)
      data.push_back(UserAuditLogSchema(row).toJson());
    co_return data;
  }
  if (filter.endTime) {
    const auto result = co_await client->execSqlCoro(
        std::string(FIND_SYNC_TO) + SyncLimits::kMaxRows, filter.userId, *filter.endTime);
    std::vector<Json::Value> data;
    for (const auto& row : result)
      data.push_back(UserAuditLogSchema(row).toJson());
    co_return data;
  }
  {
    const auto result = co_await client->execSqlCoro(std::string(FIND_SYNC_ALL) + SyncLimits::kMaxRows,
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
      co_await client->execSqlCoro(std::string(FIND_LAST_SYNC), filter.userId);
  if (result.empty())
    co_return std::nullopt;
  co_return UserAuditLogSchema(result.front()).toJson();
}

drogon::Task<std::vector<UserAuditLogCompactionPair>>
UserAuditLogRepository::findCompactionPairs(const int64_t cutoffMs) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      std::string(FIND_COMPACTION_PAIRS) + SyncLimits::kMaxRows, cutoffMs,
      cutoffMs);

  std::vector<UserAuditLogCompactionPair> pairs;
  pairs.reserve(result.size());
  for (const auto& row : result)
    pairs.push_back(
        {.olderId = static_cast<int64_t>(row["older_id"].as<long long>()),
         .newerId = static_cast<int64_t>(row["newer_id"].as<long long>())});
  co_return pairs;
}

drogon::Task<std::unordered_map<int64_t, Json::Value>>
UserAuditLogRepository::findCompactionChanges(
    const std::vector<int64_t>& ids) const
{
  std::unordered_map<int64_t, Json::Value> changes;
  if (ids.empty())
    co_return changes;

  auto client = DbService::client();
  const std::string query =
      expand(FIND_COMPACTION_CHANGES, buildInPlaceholders(ids.size()));

  std::vector<std::string> args;
  args.reserve(ids.size());
  for (const int64_t id : ids)
    args.push_back(std::to_string(id));

  const auto& argsRef = args;
  const auto result = co_await client->execSqlCoro(query, argsRef);
  for (const auto& row : result)
    changes[static_cast<int64_t>(row["id"].as<long long>())] =
        json_util::fromString(row["changes"].as<std::string>());
  co_return changes;
}

drogon::Task<int64_t> UserAuditLogRepository::findCompactionFrontier() const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      std::string(FIND_COMPACTION_FRONTIER),
      tableNameToString(TableName::UserAuditLog));
  if (result.empty())
    co_return 0;
  co_return static_cast<int64_t>(
      result.front()["compacted_through_id"].as<long long>());
}

drogon::Task<void>
UserAuditLogRepository::compactRow(const UserAuditLogCompactInput& input) const
{
  auto client = DbService::client();
  co_await client->execSqlCoro(std::string(COMPACT_ROW),
                               json_util::toString(input.changes), input.id);
}

drogon::Task<void>
UserAuditLogRepository::removeMany(const std::vector<int64_t>& ids) const
{
  if (ids.empty())
    co_return;

  auto client = DbService::client();
  const std::string query =
      expand(REMOVE_IDS, buildInPlaceholders(ids.size()));

  std::vector<std::string> args;
  args.reserve(ids.size());
  for (const int64_t id : ids)
    args.push_back(std::to_string(id));

  const auto& argsRef = args;
  co_await client->execSqlCoro(query, argsRef);
}

drogon::Task<void>
UserAuditLogRepository::advanceCompactionFrontier(
    const int64_t throughId) const
{
  auto client = DbService::client();
  co_await client->execSqlCoro(
      std::string(ADVANCE_COMPACTION_FRONTIER),
      tableNameToString(TableName::UserAuditLog), throughId);
}
