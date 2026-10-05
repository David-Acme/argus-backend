#include "device-login-challenge-repository.hxx"

#include <ctime>
#include <exception>
#include <sqlite/db-service.hxx>
#include <string>
#include <trantor/utils/Logger.h>
#include <unordered_set>

using namespace device_login_challenge_query;

bool DeviceLoginChallengeRepository::migrateLegacySchema() const
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
    return true;
  }
  catch (const std::exception& error) {
    LOG_ERROR << "device_login_challenge migration failed: " << error.what();
    return false;
  }
}

drogon::Task<DeviceLoginChallengeSchema>
DeviceLoginChallengeRepository::create(
    const DeviceLoginChallengeCreateInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      std::string(INSERT), input.challengeId, input.deviceHash, input.userAgent,
      input.expiresAt, sessionPlatformToString(input.platform),
      input.deviceName, input.pollHash, sessionOriginToString(input.origin),
      input.ipAddress);

  DeviceLoginChallengeSchema schema;
  schema.id = static_cast<int64_t>(result.insertId());
  schema.challengeId = input.challengeId;
  schema.deviceHash = input.deviceHash;
  schema.userAgent = input.userAgent;
  schema.expiresAt = input.expiresAt;
  schema.createdAt = static_cast<int64_t>(std::time(nullptr));
  schema.platform = input.platform;
  schema.deviceName = input.deviceName;
  schema.pollHash = input.pollHash;
  schema.origin = input.origin;
  schema.ipAddress = input.ipAddress;
  co_return schema;
}

drogon::Task<std::optional<DeviceLoginChallengeSchema>>
DeviceLoginChallengeRepository::findByChallengeId(
    const std::string& challengeId) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(std::string(FIND_BY_CHALLENGE_ID),
                                   challengeId);

  if (result.empty())
    co_return std::nullopt;
  co_return DeviceLoginChallengeSchema(result.front());
}

drogon::Task<bool> DeviceLoginChallengeRepository::markApproved(
    const DeviceLoginChallengeMarkApprovedInput& input) const
{
  const auto pooled = DbService::client();
  auto* client = input.client ? input.client : pooled.get();
  const auto result = co_await client->execSqlCoro(
      std::string(MARK_APPROVED), input.userId, input.accessToken,
      input.refreshToken, input.challengeId);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool>
DeviceLoginChallengeRepository::claimApproved(const std::string& challengeId,
                                              int64_t now) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(std::string(CLAIM_APPROVED),
                                                   challengeId, now);
  co_return result.affectedRows() == 1;
}

drogon::Task<bool>
DeviceLoginChallengeRepository::remove(const std::string& challengeId) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(std::string(DELETE_BY_CHALLENGE_ID),
                                   challengeId);
  co_return result.affectedRows() > 0;
}

drogon::Task<int64_t>
DeviceLoginChallengeRepository::removeExpired(int64_t now) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(std::string(DELETE_EXPIRED), now);
  co_return static_cast<int64_t>(result.affectedRows());
}
