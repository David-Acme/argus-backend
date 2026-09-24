#include "user-action-log-repository.hxx"

#include <exception>
#include <sqlite/db-service.hxx>
#include <string>
#include <string_view>
#include <text/json-util.hxx>
#include <trantor/utils/Logger.h>

using namespace user_action_log_query;

namespace
{
bool counts(std::string_view query)
{
  const auto rows = DbService::client()->execSqlSync(std::string(query));
  return !rows.empty() && rows.front()["total"].as<int64_t>() > 0;
}
}

bool UserActionLogRepository::migrateLegacySchema() const
{
  try {
    if (counts(COUNT_TABLE) && !counts(COUNT_MSG_ID_COLUMN))
      DbService::client()->execSqlSync(std::string(ADD_MSG_ID_COLUMN));
    return true;
  }
  catch (const std::exception& error) {
    LOG_ERROR << "user_action_log migration failed: " << error.what();
    return false;
  }
}

drogon::Task<std::optional<UserActionLogSchema>>
UserActionLogRepository::create(const UserActionLogCreateInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      std::string(INSERT), input.userId, input.recordId,
      tableNameToString(input.tableName), userActionToString(input.action),
      json_util::toString(input.oldData), json_util::toString(input.newData),
      input.ipAddress, input.msgId);
  if (result.affectedRows() == 0)
    co_return std::nullopt;

  UserActionLogSchema schema;
  schema.id = result.insertId();
  schema.userId = input.userId;
  schema.recordId = input.recordId;
  schema.tableName = input.tableName;
  schema.action = input.action;
  schema.oldData = input.oldData;
  schema.newData = input.newData;
  schema.ipAddress = input.ipAddress;
  schema.createdAt = std::time(nullptr);
  co_return schema;
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
