#include "sync-config.hxx"

#include <config/config-service.hxx>
#include <sync/audit-retention.hxx>

SyncDbConfig SyncConfig::resolveDb()
{
  SyncDbConfig config;
  config.dbPath = ConfigService::getString("sync.db");
  if (config.dbPath.empty())
    config.dbPath = "database/sync.db";
  config.schemaPath = ConfigService::getString("sync.schema");
  if (config.schemaPath.empty())
    config.schemaPath = "services/sync/database/schema.sql";
  return config;
}

ListenerConfig SyncConfig::resolveListener()
{
  return ListenerConfig::resolveServiceTls("sync", 7025);
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
          .notification = ConfigService::getString("notifications.grpc_target"),
          .identity = ConfigService::getString("identity.target"),
          .identitySecret = ConfigService::getString("identity.rpc_secret")};
}

int SyncConfig::resolveAuditRetentionDays()
{
  if (!ConfigService::hasKey("sync.audit_retention_days"))
    return audit_retention::kDefaultDays;
  return ConfigService::getInt("sync.audit_retention_days");
}
