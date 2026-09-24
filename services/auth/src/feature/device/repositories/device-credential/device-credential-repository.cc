#include "device-credential-repository.hxx"

#include "device-credential-query.hxx"

#include <ctime>
#include <sqlite/db-service.hxx>
#include <string>

using namespace device_credential_query;

drogon::Task<DeviceCredentialSchema> DeviceCredentialRepository::create(
    const DeviceCredentialCreateInput& input) const
{
  const auto pooled = DbService::client();
  auto* client = input.client ? input.client : pooled.get();
  const auto result =
      co_await client->execSqlCoro(std::string(INSERT), input.userId,
                                   input.deviceHash, input.secretHash);

  DeviceCredentialSchema schema;
  schema.id = static_cast<int64_t>(result.insertId());
  schema.userId = input.userId;
  schema.deviceHash = input.deviceHash;
  schema.secretHash = input.secretHash;
  schema.isActive = true;
  schema.createdAt = static_cast<int64_t>(std::time(nullptr));
  co_return schema;
}

drogon::Task<std::optional<DeviceCredentialSchema>>
DeviceCredentialRepository::findActiveBySecretHash(
    const std::string& secretHash) const
{
  auto client = DbService::client();
  const auto result = co_await client->execSqlCoro(
      std::string(FIND_ACTIVE_BY_SECRET_HASH), secretHash);

  if (result.empty())
    co_return std::nullopt;
  co_return DeviceCredentialSchema(result.front());
}
