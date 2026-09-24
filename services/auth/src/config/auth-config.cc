#include "auth-config.hxx"

#include <config/config-service.hxx>

namespace
{
constexpr int64_t kDefaultContextCacheSeconds = 30;
constexpr int kDefaultIdentityPort = 7040;
constexpr uint16_t kDefaultAuthPort = 7042;
constexpr uint16_t kDefaultRpcPort = 7043;
}

AuthDbConfig AuthConfig::resolveDb()
{
  AuthDbConfig config;
  config.dbPath = ConfigService::getString("auth.db");
  if (config.dbPath.empty())
    config.dbPath = "database/auth.db";
  config.schemaPath = ConfigService::getString("auth.schema");
  if (config.schemaPath.empty())
    config.schemaPath = "services/auth/database/schema.sql";
  return config;
}

ListenerConfig AuthConfig::resolveListener()
{
  ListenerConfig config;
  config.host = ConfigService::getString("auth.host");
  if (config.host.empty())
    config.host = "0.0.0.0";
  const int port = ConfigService::getInt("auth.port");
  config.port = port > 0 ? static_cast<uint16_t>(port) : kDefaultAuthPort;
  config.tls = !ConfigService::getBool("auth.plain");
  config.certPath = ConfigService::getString("cert.server_cert");
  if (config.certPath.empty())
    config.certPath = "certs/server.pem";
  config.keyPath = ConfigService::getString("cert.server_key");
  if (config.keyPath.empty())
    config.keyPath = "certs/server.key";
  config.minTlsProtocol = ConfigService::getString("auth.min_protocol");
  if (config.minTlsProtocol.empty())
    config.minTlsProtocol = "TLSv1.2";
  return config;
}

AuthRpcConfig AuthConfig::resolveRpc()
{
  AuthRpcConfig config;
  config.listener = GrpcListenerConfig::resolve(kDefaultRpcPort);
  config.secret = ConfigService::getString("auth.rpc_secret");
  return config;
}

bool AuthRpcConfig::reachableBeyondLoopback() const
{
  return listener.host != "127.0.0.1" && listener.host != "::1" &&
         listener.host != "localhost";
}

AuthIdentityConfig AuthConfig::resolveIdentity()
{
  AuthIdentityConfig config;
  config.target = ConfigService::getString("identity.target");
  if (config.target.empty()) {
    std::string host = ConfigService::getString("identity.rpc_host");
    if (host.empty())
      host = "127.0.0.1";
    const int port = ConfigService::getInt("identity.rpc_port");
    config.target =
        host + ":" +
        std::to_string(port > 0 ? port : kDefaultIdentityPort);
  }
  config.secret = ConfigService::getString("identity.rpc_secret");
  return config;
}

int64_t AuthConfig::resolveContextCacheSeconds()
{
  if (!ConfigService::hasKey("auth.context_cache_seconds"))
    return kDefaultContextCacheSeconds;
  const int64_t seconds = ConfigService::getInt("auth.context_cache_seconds");
  return seconds >= 0 ? seconds : kDefaultContextCacheSeconds;
}
