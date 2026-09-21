#include "user-action-log-repository.hxx"

#include <sqlite/db-service.hxx>
#include <text/json-util.hxx>

using namespace user_action_log_query;

drogon::Task<UserActionLogSchema>
UserActionLogRepository::create(const UserActionLogCreateInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      INSERT.data(), input.userId, input.recordId,
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
