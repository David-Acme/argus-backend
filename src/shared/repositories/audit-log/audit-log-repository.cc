#include "audit-log-repository.hxx"

#include <config/app-config.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/utils/json-util/json-util.hxx>
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
} // namespace

drogon::Task<AuditLogSchema>
AuditLogRepository::create(const AuditLogCreateInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      INSERT.data(),
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
      FIND_EXIST.data(), input.recordId, tableNameToString(input.tableName),
      input.dayStart, input.dayEnd);
  if (result.empty())
    co_return std::nullopt;
  co_return AuditLogSchema(result.front());
}

drogon::Task<void>
AuditLogRepository::updateChanges(const AuditLogUpdateInput& input) const
{
  auto client = DbService::client();
  co_await client->execSqlCoro(UPDATE_CHANGES.data(),
                               json_util::toString(input.changes),
                               input.eventTimestamp, input.id);
}

drogon::Task<void> AuditLogRepository::remove(int64_t id) const
{
  auto client = DbService::client();
  co_await client->execSqlCoro(REMOVE.data(), id);
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
  if (filter.startTime && filter.endTime) {
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
  query += AppConfig::SYNC_LIMIT;

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
