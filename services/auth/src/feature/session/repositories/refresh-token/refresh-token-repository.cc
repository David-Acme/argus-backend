#include "refresh-token-repository.hxx"

#include <ctime>
#include <sqlite/db-service.hxx>
#include <string>

using namespace refresh_token_query;

drogon::Task<RefreshTokenSchema>
RefreshTokenRepository::create(const RefreshTokenCreateInput& input) const
{
  const auto pooled = DbService::client();
  auto* client = input.client ? input.client : pooled.get();
  const auto result =
      co_await client->execSqlCoro(std::string(INSERT), input.userId,
                                   input.accessToken, input.refreshToken,
                                   input.deviceHash, input.userAgent,
                                   input.expiresAt);

  RefreshTokenSchema schema;
  schema.id = static_cast<int64_t>(result.insertId());
  schema.userId = input.userId;
  schema.accessToken = input.accessToken;
  schema.refreshToken = input.refreshToken;
  schema.deviceHash = input.deviceHash;
  schema.userAgent = input.userAgent;
  schema.isValid = true;
  schema.isUsed = false;
  schema.expiresAt = input.expiresAt;
  schema.createdAt = static_cast<int64_t>(std::time(nullptr));
  co_return schema;
}

drogon::Task<std::optional<RefreshTokenSchema>>
RefreshTokenRepository::findByAccessToken(int64_t userId,
                                          const std::string& accessToken) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      std::string(FIND_BY_ACCESS_TOKEN), userId, accessToken);

  if (result.empty())
    co_return std::nullopt;
  co_return RefreshTokenSchema(result.front());
}

drogon::Task<std::optional<RefreshTokenSchema>>
RefreshTokenRepository::findByRefreshToken(
    int64_t userId, const std::string& refreshToken) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      std::string(FIND_BY_REFRESH_TOKEN), userId, refreshToken);

  if (result.empty())
    co_return std::nullopt;
  co_return RefreshTokenSchema(result.front());
}

drogon::Task<bool> RefreshTokenRepository::markUsed(int64_t id) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(std::string(MARK_USED), id);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool> RefreshTokenRepository::invalidateAllUser(
    int64_t userId, drogon::orm::DbClient* client) const
{
  const auto pooled = DbService::client();
  auto* resolved = client ? client : pooled.get();
  const auto result = co_await resolved->execSqlCoro(
      std::string(INVALIDATE_ALL_USER), userId);
  co_return result.affectedRows() > 0;
}

drogon::Task<void> RefreshTokenRepository::pruneStale(int64_t userId) const
{
  auto client = DbService::client();
  co_await client->execSqlCoro(std::string(PRUNE_STALE), userId);
  co_return;
}
