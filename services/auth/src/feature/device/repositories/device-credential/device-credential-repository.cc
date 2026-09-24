#include "device-credential-repository.hxx"

#include "device-credential-query.hxx"

#include <sqlite/db-service.hxx>
#include <string>

using namespace device_credential_query;

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
