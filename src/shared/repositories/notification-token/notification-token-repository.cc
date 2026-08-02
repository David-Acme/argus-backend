#include "notification-token-repository.hxx"

#include <shared/services/sqlite/db-service.hxx>

using namespace notification_token_query;

drogon::Task<void>
NotificationTokenRepository::upsert(const NotificationTokenCreateInput& input) const
{
  auto client = DbService::client();
  co_await client->execSqlCoro(UPSERT.data(), input.userId, input.deviceHash,
                               input.token, input.platform, input.lang);
}

drogon::Task<std::vector<NotificationTokenSchema>>
NotificationTokenRepository::findByUser(int64_t userId) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(FIND_BY_USER.data(), userId);
  std::vector<NotificationTokenSchema> tokens;
  for (const auto& row : result)
    tokens.push_back(NotificationTokenSchema(row));
  co_return tokens;
}

drogon::Task<void>
NotificationTokenRepository::deleteByDevice(int64_t userId,
                                            const std::string& deviceHash) const
{
  auto client = DbService::client();
  co_await client->execSqlCoro(DELETE_BY_DEVICE.data(), userId, deviceHash);
}

drogon::Task<void>
NotificationTokenRepository::removeAllByUser(int64_t userId) const
{
  auto client = DbService::client();
  co_await client->execSqlCoro(REMOVE_ALL_BY_USER.data(), userId);
}
