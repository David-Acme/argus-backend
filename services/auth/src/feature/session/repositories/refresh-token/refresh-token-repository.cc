#include "refresh-token-repository.hxx"

#include <ctime>
#include <sqlite/db-service.hxx>
#include <string>
#include <text/sha256.hxx>

using namespace refresh_token_query;

drogon::Task<RefreshTokenSchema>
RefreshTokenRepository::create(const RefreshTokenCreateInput& input) const
{
  const auto pooled = DbService::client();
  auto* client = input.client ? input.client : pooled.get();
  const std::string accessHash = argus::hash::sha256Hex(input.accessToken);
  const std::string refreshHash = argus::hash::sha256Hex(input.refreshToken);
  const auto result =
      co_await client->execSqlCoro(std::string(INSERT), input.userId,
                                   accessHash, refreshHash, input.deviceHash,
                                   input.userAgent, input.expiresAt);

  RefreshTokenSchema schema;
  schema.id = static_cast<int64_t>(result.insertId());
  schema.userId = input.userId;
  schema.accessToken = accessHash;
  schema.refreshToken = refreshHash;
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
      std::string(FIND_BY_ACCESS_TOKEN), userId,
      argus::hash::sha256Hex(accessToken), accessToken);

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
      std::string(FIND_BY_REFRESH_TOKEN), userId,
      argus::hash::sha256Hex(refreshToken), refreshToken);

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
