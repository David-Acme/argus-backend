#include "device-login-challenge-repository.hxx"

#include <ctime>
#include <shared/services/sqlite/db-service.hxx>

using namespace device_login_challenge_query;

drogon::Task<DeviceLoginChallengeSchema>
DeviceLoginChallengeRepository::create(
    const DeviceLoginChallengeCreateInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      INSERT.data(), input.challengeId, input.deviceHash, input.userAgent,
      input.expiresAt);

  DeviceLoginChallengeSchema schema;
  schema.id = result.insertId();
  schema.challengeId = input.challengeId;
  schema.deviceHash = input.deviceHash;
  schema.userAgent = input.userAgent;
  schema.expiresAt = input.expiresAt;
  schema.createdAt = std::time(nullptr);
  co_return schema;
}

drogon::Task<std::optional<DeviceLoginChallengeSchema>>
DeviceLoginChallengeRepository::findByChallengeId(
    const std::string& challengeId) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      FIND_BY_CHALLENGE_ID.data(), challengeId);

  if (result.empty())
    co_return std::nullopt;
  co_return DeviceLoginChallengeSchema(result.front());
}

drogon::Task<bool> DeviceLoginChallengeRepository::markApproved(
    const DeviceLoginChallengeMarkApprovedInput& input) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(MARK_APPROVED.data(), input.userId,
                                   input.accessToken, input.refreshToken,
                                   input.challengeId);
  co_return result.affectedRows() > 0;
}

drogon::Task<bool>
DeviceLoginChallengeRepository::remove(const std::string& challengeId) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      DELETE_BY_CHALLENGE_ID.data(), challengeId);
  co_return result.affectedRows() > 0;
}

drogon::Task<int>
DeviceLoginChallengeRepository::deleteExpired(int64_t now) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(DELETE_EXPIRED.data(), now);
  co_return static_cast<int>(result.affectedRows());
}
