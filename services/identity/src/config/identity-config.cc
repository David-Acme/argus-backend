#include "identity-config.hxx"

#include <config/config-service.hxx>
#include <cstdint>

namespace
{
constexpr uint16_t kDefaultIdentityPort = 7044;
constexpr uint16_t kDefaultRpcPort = 7040;
}

IdentityDbConfig IdentityConfig::resolveDb()
{
  IdentityDbConfig config;
  config.dbPath = ConfigService::getString("identity.db");
  if (config.dbPath.empty())
    config.dbPath = "database/identity.db";
  config.schemaPath = ConfigService::getString("identity.schema");
  if (config.schemaPath.empty())
    config.schemaPath = "services/identity/database/schema.sql";
  return config;
}

ListenerConfig IdentityConfig::resolveListener()
{
  return ListenerConfig::resolveServiceTls("identity", kDefaultIdentityPort);
}

uint16_t IdentityConfig::resolveAnnouncedPort()
{
  return resolveListener().port;
}

IdentityRpcConfig IdentityConfig::resolveRpc()
{
  IdentityRpcConfig config;
  config.listener = GrpcListenerConfig::resolve(kDefaultRpcPort);
  config.secret = ConfigService::getString("identity.rpc_secret");
  return config;
}

bool IdentityRpcConfig::reachableBeyondLoopback() const
{
  return listener.host != "127.0.0.1" && listener.host != "::1" &&
         listener.host != "localhost";
}

IdentitySyncControlConfig IdentityConfig::resolveSyncControl()
{
  IdentitySyncControlConfig config;
  config.target = ConfigService::getString("sync.control_target");
  config.secret = ConfigService::getString("sync.control_secret");
  return config;
}

IdentityFaceConfig IdentityConfig::resolveFace()
{
  IdentityFaceConfig config;
  config.enabled = ConfigService::getBool("face.enabled");
  return config;
}
