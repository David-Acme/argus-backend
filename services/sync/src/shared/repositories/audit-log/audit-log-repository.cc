#include "audit-log-repository.hxx"

#include <sync/sync-limits.hxx>
#include <sqlite/db-service.hxx>
#include <text/json-util.hxx>
#include <string>

using namespace audit_log_query;

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

void appendTableNames(std::vector<std::string>& args,
                      const std::vector<TableName>& tables)
{
  for (const auto table : tables)
    args.push_back(tableNameToString(table));
}
}

drogon::Task<AuditLogSchema>
AuditLogRepository::create(const AuditLogCreateInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      std::string(INSERT),
      input.createUserId ? std::optional<int64_t>(*input.createUserId)
                         : std::optional<int64_t>{},
      input.recordId, tableNameToString(input.tableName),
      json_util::toString(input.changes), static_cast<int>(input.priority),
      input.eventTimestamp);

  AuditLogSchema schema;
  schema.id = result.insertId();
  schema.createUserId = input.createUserId;
  schema.recordId = input.recordId;
  schema.tableName = input.tableName;
  schema.changes = input.changes;
  schema.priority = input.priority;
  schema.eventTimestamp = input.eventTimestamp;
  schema.createdAt = std::time(nullptr);
  co_return schema;
}

drogon::Task<std::optional<AuditLogSchema>>
AuditLogRepository::findExist(const AuditLogFindExistInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      std::string(FIND_EXIST), input.recordId,
      tableNameToString(input.tableName), input.dayStart, input.dayEnd);
  if (result.empty())
    co_return std::nullopt;
  co_return AuditLogSchema(result.front());
}

drogon::Task<void> AuditLogRepository::remove(int64_t id) const
{
  auto client = DbService::client();
  co_await client->execSqlCoro(std::string(REMOVE), id);
}

drogon::Task<std::vector<Json::Value>>
AuditLogRepository::findSync(const AuditLogSyncFilter& filter) const
{
  if (filter.tableNames.empty())
    co_return {};

  auto client = DbService::client();
  const std::string placeholders = buildInPlaceholders(filter.tableNames.size());

  std::vector<std::string> args;
  appendTableNames(args, filter.tableNames);

  std::string query;
  if (filter.afterId) {
    query = expand(filter.endId ? FIND_SYNC_AFTER_ID_TO : FIND_SYNC_AFTER_ID,
                   placeholders);
    args.push_back(std::to_string(*filter.afterId));
    if (filter.endId)
      args.push_back(std::to_string(*filter.endId));
  }
  else if (filter.startTime && filter.endTime) {
    query = expand(FIND_SYNC, placeholders);
    args.push_back(std::to_string(*filter.startTime));
    args.push_back(std::to_string(*filter.endTime));
  }
  else if (filter.startTime) {
    query = expand(FIND_SYNC_FROM, placeholders);
    args.push_back(std::to_string(*filter.startTime));
  }
  else if (filter.endTime) {
    query = expand(FIND_SYNC_TO, placeholders);
    args.push_back(std::to_string(*filter.endTime));
  }
  else {
    query = expand(FIND_SYNC_ALL, placeholders);
  }
  query += SyncLimits::kMaxRows;

  const auto& argsRef = args;
  const auto result = co_await client->execSqlCoro(query, argsRef);

  std::vector<Json::Value> data;
  for (const auto& row : result)
    data.push_back(AuditLogSchema(row).toJson());
  co_return data;
}

drogon::Task<std::optional<Json::Value>>
AuditLogRepository::findLastSync(const AuditLogSyncFilter& filter) const
{
  if (filter.tableNames.empty())
    co_return std::nullopt;

  auto client = DbService::client();
  const std::string placeholders = buildInPlaceholders(filter.tableNames.size());

  std::vector<std::string> args;
  appendTableNames(args, filter.tableNames);

  const auto& argsRef = args;
  const auto result =
      co_await client->execSqlCoro(expand(FIND_LAST_SYNC, placeholders),
                                   argsRef);
  if (result.empty())
    co_return std::nullopt;
  co_return AuditLogSchema(result.front()).toJson();
}

drogon::Task<std::vector<AuditLogCompactionPair>>
AuditLogRepository::findCompactionPairs(const int64_t cutoffMs) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      std::string(FIND_COMPACTION_PAIRS) + SyncLimits::kMaxRows, cutoffMs,
      cutoffMs);

  std::vector<AuditLogCompactionPair> pairs;
  pairs.reserve(result.size());
  for (const auto& row : result)
    pairs.push_back(
        {.olderId = static_cast<int64_t>(row["older_id"].as<long long>()),
         .newerId = static_cast<int64_t>(row["newer_id"].as<long long>())});
  co_return pairs;
}

drogon::Task<std::unordered_map<int64_t, Json::Value>>
AuditLogRepository::findCompactionChanges(
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

drogon::Task<int64_t> AuditLogRepository::findCompactionFrontier() const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      std::string(FIND_COMPACTION_FRONTIER),
      tableNameToString(TableName::AuditLog));
  if (result.empty())
    co_return 0;
  co_return static_cast<int64_t>(
      result.front()["compacted_through_id"].as<long long>());
}

drogon::Task<void>
AuditLogRepository::compactRow(const AuditLogCompactInput& input) const
{
  auto client = DbService::client();
  co_await client->execSqlCoro(std::string(COMPACT_ROW),
                               json_util::toString(input.changes), input.id);
}

drogon::Task<void>
AuditLogRepository::removeMany(const std::vector<int64_t>& ids) const
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
AuditLogRepository::advanceCompactionFrontier(const int64_t throughId) const
{
  auto client = DbService::client();
  co_await client->execSqlCoro(
      std::string(ADVANCE_COMPACTION_FRONTIER),
      tableNameToString(TableName::AuditLog), throughId);
}
