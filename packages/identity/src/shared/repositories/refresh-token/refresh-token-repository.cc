#include "refresh-token-repository.hxx"

#include <ctime>
#include <shared/services/sqlite/db-service.hxx>

using namespace refresh_token_query;

drogon::Task<RefreshTokenSchema>
RefreshTokenRepository::create(const RefreshTokenCreateInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(

      INSERT.data(), input.userId, input.accessToken, input.refreshToken,
      input.deviceHash, input.userAgent, input.expiresAt);

  RefreshTokenSchema schema;
  schema.id = result.insertId();
  schema.userId = input.userId;
  schema.accessToken = input.accessToken;
  schema.refreshToken = input.refreshToken;
  schema.deviceHash = input.deviceHash;
  schema.userAgent = input.userAgent;
  schema.isValid = true;
  schema.isUsed = false;
  schema.expiresAt = input.expiresAt;
  schema.createdAt = std::time(nullptr);
  co_return schema;
}

drogon::Task<std::optional<RefreshTokenSchema>>
RefreshTokenRepository::findById(int64_t id) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(

      FIND_BY_ID.data(), id);

  if (result.empty())
    co_return std::nullopt;
  co_return RefreshTokenSchema(result.front());
}

drogon::Task<std::optional<RefreshTokenSchema>>
RefreshTokenRepository::findByAccessToken(int64_t userId,
                                          const std::string& accessToken) const
{
  auto client = DbService::identityClient();
  const auto result = co_await client->execSqlCoro(

      FIND_BY_ACCESS_TOKEN.data(), userId, accessToken);

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

      FIND_BY_REFRESH_TOKEN.data(), userId, refreshToken);

  if (result.empty())
    co_return std::nullopt;
  co_return RefreshTokenSchema(result.front());
}

drogon::Task<bool> RefreshTokenRepository::hasActiveSession(
    int64_t userId, const std::string& deviceHash) const
{
  if (userId <= 0)
    co_return false;
  auto client = DbService::client();
  const int64_t now = static_cast<int64_t>(std::time(nullptr));
  const auto result =
      deviceHash.empty()
          ? co_await client->execSqlCoro(COUNT_ACTIVE_SESSIONS.data(), userId,
                                         now)
          : co_await client->execSqlCoro(
                COUNT_ACTIVE_SESSIONS_FOR_DEVICE.data(), userId, deviceHash,
                now);
  co_return !result.empty() && result.front()["total"].as<int64_t>() > 0;
}

drogon::Task<bool> RefreshTokenRepository::invalidate(int64_t id) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(

      INVALIDATE.data(), id);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool> RefreshTokenRepository::markUsed(int64_t id) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(

      MARK_USED.data(), id);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool>
RefreshTokenRepository::invalidateAllUser(int64_t userId) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(

      INVALIDATE_ALL_USER.data(), userId);
  co_return result.affectedRows() > 0;
}

drogon::Task<void> RefreshTokenRepository::pruneStale(int64_t userId) const
{
  auto client = DbService::client();
  co_await client->execSqlCoro(PRUNE_STALE.data(), userId);
  co_return;
}
