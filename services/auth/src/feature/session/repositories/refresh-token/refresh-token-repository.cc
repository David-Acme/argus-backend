#include "refresh-token-repository.hxx"

#include "refresh-token-query.hxx"

#include <sqlite/db-service.hxx>
#include <string>

using namespace refresh_token_query;

drogon::Task<std::optional<RefreshTokenSchema>>
RefreshTokenRepository::findByAccessToken(
    int64_t userId, const std::string& accessToken) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      std::string(FIND_BY_ACCESS_TOKEN), userId, accessToken);

  if (result.empty())
    co_return std::nullopt;
  co_return RefreshTokenSchema(result.front());
}

drogon::Task<bool> RefreshTokenRepository::invalidateAllUser(int64_t userId) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(std::string(INVALIDATE_ALL_USER), userId);
  co_return result.affectedRows() > 0;
}
