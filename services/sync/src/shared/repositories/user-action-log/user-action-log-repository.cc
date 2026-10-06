#include "user-action-log-repository.hxx"

#include <exception>
#include <shared/vocabulary/module-tables.hxx>
#include <span>
#include <sqlite/db-service.hxx>
#include <string>
#include <string_view>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>
#include <utility>

using namespace user_action_log_query;

namespace
{
bool counts(std::string_view query)
{
  const auto rows = DbService::client()->execSqlSync(std::string(query));
  return !rows.empty() && rows.front()["total"].as<int64_t>() > 0;
}

std::string quotedTables(std::span<const TableName> tables)
{
  std::string list;
  for (const auto table : tables) {
    if (!list.empty())
      list += ", ";
    list += "'" + tableNameToString(table) + "'";
  }
  return list;
}

std::string backfillStatement()
{
  std::string sql(BACKFILL_MODULE_PREFIX);
  for (const auto module : {module_tables::kSurveillance, module_tables::kProductivity}) {
    sql += BACKFILL_MODULE_WHEN;
    sql += quotedTables(module_tables::tablesOf(module));
    sql += BACKFILL_MODULE_THEN;
    sql += "'" + std::string(module) + "' ";
  }
  sql += BACKFILL_MODULE_SUFFIX;
  return sql;
}

struct ActivityQuery
{
  std::string sql{ACTIVITY_SELECT};
  std::vector<std::string> args;
  bool filtered{false};

  void add(std::string_view condition)
  {
    sql += filtered ? ACTIVITY_AND : ACTIVITY_WHERE;
    sql += condition;
    filtered = true;
  }
};

ActivityQuery activityQuery(const ActivityListInput& input)
{
  const ActivityFilter& filter = input.filter;
  ActivityQuery query;
  if (!filter.module.empty()) {
    query.add(ACTIVITY_MODULE);
    query.args.push_back(filter.module);
  }
  if (filter.userId > 0) {
    query.add(ACTIVITY_USER);
    query.args.push_back(std::to_string(filter.userId));
  }
  if (!filter.action.empty()) {
    query.add(ACTIVITY_ACTION);
    query.args.push_back(filter.action);
  }
  if (!filter.table.empty()) {
    query.add(ACTIVITY_TABLE);
    query.args.push_back(filter.table);
  }
  if (filter.from > 0) {
    query.add(ACTIVITY_FROM);
    query.args.push_back(std::to_string(filter.from));
  }
  if (filter.to > 0) {
    query.add(ACTIVITY_TO);
    query.args.push_back(std::to_string(filter.to));
  }
  if (input.after) {
    query.add(ACTIVITY_CURSOR);
    query.args.push_back(std::to_string(input.after->createdAt));
    query.args.push_back(std::to_string(input.after->createdAt));
    query.args.push_back(std::to_string(input.after->id));
  }
  query.sql += ACTIVITY_ORDER;
  query.sql += std::to_string(input.limit);
  return query;
}
}

bool UserActionLogRepository::migrateLegacySchema() const
{
  try {
    if (counts(COUNT_TABLE)) {
      if (!counts(COUNT_MSG_ID_COLUMN))
        DbService::client()->execSqlSync(std::string(ADD_MSG_ID_COLUMN));
      if (!counts(COUNT_MODULE_COLUMN))
        DbService::client()->execSqlSync(std::string(ADD_MODULE_COLUMN));
    }
    return true;
  }
  catch (const std::exception& error) {
    LOG_ERROR << "user_action_log migration failed: " << error.what();
    return false;
  }
}

std::string UserActionLogRepository::activityStatement(const ActivityListInput& input)
{
  return activityQuery(input).sql;
}

bool UserActionLogRepository::backfillModules() const
{
  try {
    DbService::client()->execSqlSync(backfillStatement());
    return true;
  }
  catch (const std::exception& error) {
    LOG_ERROR << "user_action_log module backfill failed: " << error.what();
    return false;
  }
}

drogon::Task<std::optional<UserActionLogSchema>>
UserActionLogRepository::create(const UserActionLogCreateInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      std::string(INSERT), input.userId, input.recordId, input.tableName,
      input.module, userActionToString(input.action),
      json_util::toString(input.oldData), json_util::toString(input.newData),
      input.ipAddress, input.msgId);
  if (result.affectedRows() == 0)
    co_return std::nullopt;

  UserActionLogSchema schema;
  schema.id = static_cast<int64_t>(result.insertId());
  schema.userId = input.userId;
  schema.recordId = input.recordId;
  schema.tableName = input.tableName;
  schema.module = input.module;
  schema.action = input.action;
  schema.oldData = input.oldData;
  schema.newData = input.newData;
  schema.ipAddress = input.ipAddress;
  schema.createdAt = std::time(nullptr);
  co_return schema;
}

drogon::Task<std::vector<UserActionLogSchema>>
UserActionLogRepository::list(const ActivityListInput& input) const
{
  auto client = DbService::client();
  const ActivityQuery query = activityQuery(input);
  const auto& argsRef = query.args;
  const auto rows = co_await client->execSqlCoro(query.sql, argsRef);

  std::vector<UserActionLogSchema> data;
  data.reserve(rows.size());
  for (const auto& row : rows)
    data.emplace_back(row);
  co_return data;
}

drogon::Task<std::vector<Json::Value>>
UserActionLogRepository::find(const SyncFilter& filter) const
{
  auto client = DbService::client();
  const auto [query, args] =
      sync_query::buildSyncQuery({.filter = filter,
                                  .queryBoth = SYNC_FIND,
                                  .queryFrom = SYNC_FIND_FROM,
                                  .queryAll = SYNC_FIND_ALL,
                                  .queryAfterBoth = SYNC_FIND_AFTER,
                                  .queryAfterFrom = SYNC_FIND_AFTER_FROM});
  const auto& argsRef = args;
  const auto rows = co_await client->execSqlCoro(query, argsRef);

  std::vector<Json::Value> data;
  for (const auto& row : rows)
    data.push_back(UserActionLogSchema(row).toJson());
  co_return data;
}

drogon::Task<std::vector<Json::Value>>
UserActionLogRepository::findDeleted(const SyncFilter&) const
{
  co_return {};
}

drogon::Task<std::optional<Json::Value>>
UserActionLogRepository::findLast(const SyncFilter&) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(std::string(SYNC_FIND_LAST));
  if (result.empty())
    co_return std::nullopt;
  co_return UserActionLogSchema(result.front()).toJson();
}

drogon::Task<std::optional<Json::Value>>
UserActionLogRepository::findLastDeleted(const SyncFilter&) const
{
  co_return std::nullopt;
}
