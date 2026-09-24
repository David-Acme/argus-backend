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
  ListenerConfig config;
  config.host = ConfigService::getString("identity.host");
  if (config.host.empty())
    config.host = "0.0.0.0";
  const int port = ConfigService::getInt("identity.port");
  config.port = port > 0 ? static_cast<uint16_t>(port) : kDefaultIdentityPort;
  config.tls = !ConfigService::getBool("identity.plain");
  config.certPath = ConfigService::getString("cert.server_cert");
  if (config.certPath.empty())
    config.certPath = "certs/server.pem";
  config.keyPath = ConfigService::getString("cert.server_key");
  if (config.keyPath.empty())
    config.keyPath = "certs/server.key";
  config.minTlsProtocol = ConfigService::getString("identity.min_protocol");
  if (config.minTlsProtocol.empty())
    config.minTlsProtocol = "TLSv1.2";
  return config;
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
