#include "user-audit-log-repository.hxx"

#include <ctime>
#include <sync/sync-limits.hxx>
#include <sqlite/db-service.hxx>
#include <text/json-util.hxx>
#include <unordered_map>

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

drogon::Task<std::vector<UserAuditLogSchema>>
UserAuditLogRepository::findExistMany(
    const UserAuditLogFindExistManyInput& input) const
{
  if (input.userIds.empty())
    co_return {};

  const auto pooled = DbService::client();
  auto* client = input.client ? input.client : pooled.get();

  std::vector<std::string> args;
  args.reserve(input.userIds.size() + 4);
  args.push_back(std::to_string(input.recordId));
  args.push_back(tableNameToString(input.tableName));
  for (const int64_t userId : input.userIds)
    args.push_back(std::to_string(userId));
  args.push_back(std::to_string(input.dayStart));
  args.push_back(std::to_string(input.dayEnd));

  const std::string query =
      expand(FIND_EXIST_MANY, buildInPlaceholders(input.userIds.size()));
  const auto& argsRef = args;
  const auto result = co_await client->execSqlCoro(query, argsRef);

  std::vector<UserAuditLogSchema> rows;
  rows.reserve(result.size());
  for (const auto& row : result)
    rows.emplace_back(row);
  co_return rows;
}

drogon::Task<std::vector<UserAuditLogSchema>>
UserAuditLogRepository::createMany(
    const UserAuditLogCreateManyInput& input) const
{
  if (input.rows.empty())
    co_return {};

  const auto pooled = DbService::client();
  auto* client = input.client ? input.client : pooled.get();

  std::string values;
  values.reserve(input.rows.size() * (INSERT_MANY_ROW.size() + 2));
  std::vector<std::string> args;
  args.reserve(input.rows.size() * 6);
  std::unordered_map<int64_t, std::size_t> byUser;
  byUser.reserve(input.rows.size());
  for (std::size_t i = 0; i < input.rows.size(); ++i) {
    const auto& row = input.rows[i];
    if (i > 0)
      values += ", ";
    values += INSERT_MANY_ROW;
    args.push_back(std::to_string(row.userId));
    args.push_back(std::to_string(row.recordId));
    args.push_back(tableNameToString(row.tableName));
    args.push_back(json_util::toString(row.changes));
    args.push_back(std::to_string(static_cast<int>(row.priority)));
    args.push_back(std::to_string(row.eventTimestamp));
    byUser.emplace(row.userId, i);
  }

  const auto& argsRef = args;
  const auto result =
      co_await client->execSqlCoro(expand(INSERT_MANY, values), argsRef);

  const auto createdAt = std::time(nullptr);
  std::vector<UserAuditLogSchema> written(input.rows.size());
  for (const auto& inserted : result) {
    const auto userId =
        static_cast<int64_t>(inserted["user_id"].as<long long>());
    const auto found = byUser.find(userId);
    if (found == byUser.end())
      continue;
    const auto& row = input.rows[found->second];
    auto& schema = written[found->second];
    schema.id = static_cast<int64_t>(inserted["id"].as<long long>());
    schema.userId = row.userId;
    schema.recordId = row.recordId;
    schema.tableName = row.tableName;
    schema.changes = row.changes;
    schema.priority = row.priority;
    schema.eventTimestamp = row.eventTimestamp;
    schema.createdAt = createdAt;
  }
  co_return written;
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
UserAuditLogRepository::findCompactionPairs(
    const UserAuditLogCompactionWindow& window) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      std::string(FIND_COMPACTION_PAIRS) + SyncLimits::kMaxRows,
      window.afterId, window.cutoffMs, window.cutoffMs);

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
  const auto pooled = DbService::client();
  auto* client = input.client ? input.client : pooled.get();
  co_await client->execSqlCoro(std::string(COMPACT_ROW),
                               json_util::toString(input.changes), input.id);
}

drogon::Task<void>
UserAuditLogRepository::removeMany(const std::vector<int64_t>& ids,
                                drogon::orm::DbClient* resolvedClient) const
{
  if (ids.empty())
    co_return;

  const auto pooled = DbService::client();
  auto* client = resolvedClient ? resolvedClient : pooled.get();
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
    int64_t throughId, drogon::orm::DbClient* resolvedClient) const
{
  const auto pooled = DbService::client();
  auto* client = resolvedClient ? resolvedClient : pooled.get();
  co_await client->execSqlCoro(
      std::string(ADVANCE_COMPACTION_FRONTIER),
      tableNameToString(TableName::UserAuditLog), throughId);
}
