#include "sync-config.hxx"

#include <config/config-service.hxx>
#include <sync/audit-retention.hxx>

SyncDbConfig SyncConfig::resolveDb()
{
  SyncDbConfig config;
  config.dbPath = ConfigService::getString("sync.db");
  if (config.dbPath.empty())
    config.dbPath = "database/identity.db";
  config.schemaPath = ConfigService::getString("sync.schema");
  if (config.schemaPath.empty())
    config.schemaPath = "services/sync/database/schema.sql";
  return config;
}

ListenerConfig SyncConfig::resolveListener()
{
  ListenerConfig config;
  config.host = ConfigService::getString("sync.host");
  if (config.host.empty())
    config.host = "0.0.0.0";
  const int port = ConfigService::getInt("sync.port");
  config.port = port > 0 ? static_cast<uint16_t>(port) : 7025;
  config.tls = !ConfigService::getBool("sync.plain");
  config.certPath = ConfigService::getString("cert.server_cert");
  if (config.certPath.empty())
    config.certPath = "certs/server.pem";
  config.keyPath = ConfigService::getString("cert.server_key");
  if (config.keyPath.empty())
    config.keyPath = "certs/server.key";
  config.minTlsProtocol = ConfigService::getString("sync.min_protocol");
  if (config.minTlsProtocol.empty())
    config.minTlsProtocol = "TLSv1.2";
  return config;
}

SyncControlConfig SyncConfig::resolveControl()
{
  SyncControlConfig config;
  config.listener = GrpcListenerConfig::resolve(7041);
  config.secret = ConfigService::getString("sync.control_secret");
  return config;
}

bool SyncControlConfig::reachableBeyondLoopback() const
{
  return listener.host != "127.0.0.1" && listener.host != "::1" &&
         listener.host != "localhost";
}

SyncUpstreams SyncConfig::resolveUpstreams()
{
  return {.camera = ConfigService::getString("camera.grpc_target"),
          .productivity = ConfigService::getString("productivity.grpc_target"),
          .notification = ConfigService::getString("notifications.grpc_target")};
}

int SyncConfig::resolveAuditRetentionDays()
{
  if (!ConfigService::hasKey("sync.audit_retention_days"))
    return audit_retention::kDefaultDays;
  return ConfigService::getInt("sync.audit_retention_days");
}
