#include "user-action-log-repository.hxx"

#include <sqlite/db-service.hxx>
#include <string>
#include <text/json-util.hxx>

using namespace user_action_log_query;

drogon::Task<UserActionLogSchema>
UserActionLogRepository::create(const UserActionLogCreateInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      std::string(INSERT), input.userId, input.recordId,
      tableNameToString(input.tableName), userActionToString(input.action),
      json_util::toString(input.oldData), json_util::toString(input.newData),
      input.ipAddress);

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

// Insert-only: the journal has no tombstone, so nothing is ever found deleted.
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
