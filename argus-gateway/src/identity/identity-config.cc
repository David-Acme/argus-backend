#include "identity-config.hxx"

#include <cstdint>

#include <shared/services/config-service/config-service.hxx>

IdentityDbConfig IdentityConfig::resolveDb()
{
  IdentityDbConfig config;
  config.dbPath = ConfigService::getString("identity.db");
  if (config.dbPath.empty())
    config.dbPath = "database/identity.db";
  config.schemaPath = ConfigService::getString("identity.schema");
  if (config.schemaPath.empty())
    config.schemaPath = "argus-identity/database/schema.sql";
  return config;
}

IdentityRpcConfig IdentityRpcConfig::resolve()
{
  IdentityRpcConfig config;
  config.host = ConfigService::getString("identity.rpc_host");
  if (config.host.empty())
    config.host = "127.0.0.1";
  const int port = ConfigService::getInt("identity.rpc_port");
  config.port = port > 0 ? static_cast<uint16_t>(port) : 7040;
  config.secret = ConfigService::getString("identity.rpc_secret");
  return config;
}

bool IdentityRpcConfig::reachableBeyondLoopback() const
{
  return host != "127.0.0.1" && host != "::1" && host != "localhost";
}
