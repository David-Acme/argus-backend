#include "device-credential-repository.hxx"

#include <ctime>
#include <shared/services/sqlite/db-service.hxx>

using namespace device_credential_query;

drogon::Task<DeviceCredentialSchema>
DeviceCredentialRepository::create(const DeviceCredentialCreateInput& input) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      INSERT.data(), input.userId, input.deviceHash, input.secretHash);

  DeviceCredentialSchema schema;
  schema.id = result.insertId();
  schema.userId = input.userId;
  schema.deviceHash = input.deviceHash;
  schema.secretHash = input.secretHash;
  schema.isActive = true;
  schema.createdAt = std::time(nullptr);
  co_return schema;
}

drogon::Task<std::optional<DeviceCredentialSchema>>
DeviceCredentialRepository::findActiveBySecretHash(
    const std::string& secretHash) const
{
  auto client = DbService::identityClient();
  const auto result = co_await client->execSqlCoro(
      FIND_ACTIVE_BY_SECRET_HASH.data(), secretHash);

  if (result.empty())
    co_return std::nullopt;
  co_return DeviceCredentialSchema(result.front());
}
