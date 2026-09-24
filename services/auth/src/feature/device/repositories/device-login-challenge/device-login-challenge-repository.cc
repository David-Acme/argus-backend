#include "device-login-challenge-repository.hxx"

#include <ctime>
#include <sqlite/db-service.hxx>
#include <string>

using namespace device_login_challenge_query;

drogon::Task<DeviceLoginChallengeSchema>
DeviceLoginChallengeRepository::create(
    const DeviceLoginChallengeCreateInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      std::string(INSERT), input.challengeId, input.deviceHash, input.userAgent,
      input.expiresAt);

  DeviceLoginChallengeSchema schema;
  schema.id = static_cast<int64_t>(result.insertId());
  schema.challengeId = input.challengeId;
  schema.deviceHash = input.deviceHash;
  schema.userAgent = input.userAgent;
  schema.expiresAt = input.expiresAt;
  schema.createdAt = static_cast<int64_t>(std::time(nullptr));
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
DeviceLoginChallengeRepository::remove(const std::string& challengeId) const
{
  auto client = DbService::client();
  const auto result =
      co_await client->execSqlCoro(std::string(DELETE_BY_CHALLENGE_ID),
                                   challengeId);
  co_return result.affectedRows() > 0;
}
