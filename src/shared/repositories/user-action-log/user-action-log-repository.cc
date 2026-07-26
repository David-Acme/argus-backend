#include "user-action-log-repository.hxx"

#include <shared/services/sqlite/db-service.hxx>
using namespace user_action_log_query;

drogon::Task<std::vector<UserActionLogSchema>>
UserActionLogRepository::findByUser(int64_t userId, int32_t limit) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(FIND_BY_USER.data(), userId, limit);
  std::vector<UserActionLogSchema> logs;
  for (const auto& row : result)
    logs.push_back(UserActionLogSchema(row));
  co_return logs;
}

drogon::Task<std::vector<UserActionLogSchema>>
UserActionLogRepository::findByRecord(int64_t recordId,
                                      const std::string& tableName,
                                      int32_t limit) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_BY_RECORD.data(),
                                                   recordId, tableName, limit);
  std::vector<UserActionLogSchema> logs;
  for (const auto& row : result)
    logs.push_back(UserActionLogSchema(row));
  co_return logs;
}
