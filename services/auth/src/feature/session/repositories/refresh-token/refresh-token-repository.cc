#include "refresh-token-repository.hxx"

#include <ctime>
#include <exception>
#include <sqlite/db-service.hxx>
#include <string>
#include <text/sha256.hxx>
#include <trantor/utils/Logger.h>
#include <unordered_set>

using namespace refresh_token_query;

namespace
{
drogon::orm::DbClient* resolve(drogon::orm::DbClient* client,
                               const drogon::orm::DbClientPtr& pooled)
{
  return client != nullptr ? client : pooled.get();
}
}

bool RefreshTokenRepository::migrateLegacySchema() const
{
  try {
    const auto client = DbService::client();
    const auto tables = client->execSqlSync(std::string(COUNT_TABLE));
    if (tables.empty() || tables.front()["total"].as<int64_t>() == 0)
      return true;

    std::unordered_set<std::string> present;
    for (const auto& row : client->execSqlSync(std::string(TABLE_COLUMNS)))
      present.insert(row["name"].as<std::string>());
    for (const auto& column : ADDED_COLUMNS) {
      if (!present.contains(std::string(column.name)))
        client->execSqlSync(std::string(column.statement));
    }

    const auto adopted =
        client->execSqlSync(std::string(BACKFILL_LEGACY_SESSIONS));
    if (adopted.affectedRows() > 0)
      LOG_INFO << "refresh_token migration: " << adopted.affectedRows()
               << " legacy row(s) received a session id";
    return true;
  }
  catch (const std::exception& error) {
    LOG_ERROR << "refresh_token migration failed: " << error.what();
    return false;
  }
}

drogon::Task<RefreshTokenSchema>
RefreshTokenRepository::create(const RefreshTokenCreateInput& input) const
{
  const auto pooled = DbService::client();
  auto* client = resolve(input.client, pooled);
  const auto now = static_cast<int64_t>(std::time(nullptr));
  const int64_t sessionCreatedAt =
      input.sessionCreatedAt > 0 ? input.sessionCreatedAt : now;
  const std::string accessHash = argus::hash::sha256Hex(input.accessToken);
  const std::string refreshHash = argus::hash::sha256Hex(input.refreshToken);
  const std::string platform = sessionPlatformToString(input.platform);
  const auto result = co_await client->execSqlCoro(
      std::string(INSERT), input.userId, accessHash, refreshHash,
      input.deviceHash, input.userAgent, input.expiresAt, now, input.sessionId,
      platform, input.deviceName, sessionCreatedAt, now,
      input.previousRefreshHash);

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
  schema.createdAt = now;
  schema.sessionId = input.sessionId;
  schema.platform = input.platform;
  schema.deviceName = input.deviceName;
  schema.sessionCreatedAt = sessionCreatedAt;
  schema.lastSeenAt = now;
  schema.previousRefreshToken = input.previousRefreshHash;
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

drogon::Task<std::optional<RefreshTokenSchema>>
RefreshTokenRepository::findActiveBySession(
    const SessionLookupInput& input) const
{
  const auto pooled = DbService::client();
  auto* client = resolve(input.client, pooled);
  const auto result = co_await client->execSqlCoro(
      std::string(FIND_ACTIVE_BY_SESSION), input.userId, input.sessionId);
  if (result.empty())
    co_return std::nullopt;
  co_return RefreshTokenSchema(result.front());
}

drogon::Task<std::optional<RefreshTokenSchema>>
RefreshTokenRepository::findActiveByPrevious(
    int64_t userId, const std::string& previousHash) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      std::string(FIND_ACTIVE_BY_PREVIOUS), userId, previousHash);
  if (result.empty())
    co_return std::nullopt;
  co_return RefreshTokenSchema(result.front());
}

drogon::Task<std::vector<RefreshTokenSchema>>
RefreshTokenRepository::listActive(const ActiveSessionsInput& input) const
{
  const auto pooled = DbService::client();
  auto* client = resolve(input.client, pooled);
  const auto result = co_await client->execSqlCoro(std::string(LIST_ACTIVE),
                                                   input.userId, input.now);
  std::vector<RefreshTokenSchema> sessions;
  sessions.reserve(result.size());
  for (const auto& row : result)
    sessions.emplace_back(row);
  co_return sessions;
}

drogon::Task<bool>
RefreshTokenRepository::markUsed(int64_t id, drogon::orm::DbClient* client) const
{
  const auto pooled = DbService::client();
  auto* resolved = resolve(client, pooled);
  const auto result =
      co_await resolved->execSqlCoro(std::string(MARK_USED), id);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool> RefreshTokenRepository::invalidateAllUser(
    int64_t userId, drogon::orm::DbClient* client) const
{
  const auto pooled = DbService::client();
  auto* resolved = resolve(client, pooled);
  const auto result = co_await resolved->execSqlCoro(
      std::string(INVALIDATE_ALL_USER), userId);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool>
RefreshTokenRepository::invalidateSession(const SessionLookupInput& input) const
{
  const auto pooled = DbService::client();
  auto* client = resolve(input.client, pooled);
  const auto result = co_await client->execSqlCoro(
      std::string(INVALIDATE_SESSION), input.userId, input.sessionId);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool> RefreshTokenRepository::invalidateOtherSessions(
    const SessionLookupInput& input) const
{
  const auto pooled = DbService::client();
  auto* client = resolve(input.client, pooled);
  const auto result = co_await client->execSqlCoro(
      std::string(INVALIDATE_OTHER_SESSIONS), input.userId, input.sessionId);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool>
RefreshTokenRepository::touch(const SessionTouchInput& input) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(std::string(TOUCH), input.now, input.rowId,
                                   input.now - input.throttleSeconds);
  co_return result.affectedRows() > 0;
}

drogon::Task<void>
RefreshTokenRepository::adoptLegacySessions(int64_t userId) const
{
  auto client = DbService::client();
  co_await client->execSqlCoro(std::string(ADOPT_LEGACY_SESSIONS), userId);
}

drogon::Task<void>
RefreshTokenRepository::pruneStale(int64_t userId,
                                   drogon::orm::DbClient* client) const
{
  const auto pooled = DbService::client();
  auto* resolved = resolve(client, pooled);
  co_await resolved->execSqlCoro(std::string(PRUNE_STALE), userId);
}
