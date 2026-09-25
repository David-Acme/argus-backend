#include "notification-config.hxx"

#include <config/config-service.hxx>

NotificationDbConfig NotificationConfig::resolveDb()
{
  NotificationDbConfig config;
  config.dbPath = ConfigService::getString("notifications.db");
  if (config.dbPath.empty())
    config.dbPath = "database/notification.db";
  config.schemaPath = ConfigService::getString("notifications.schema");
  if (config.schemaPath.empty())
    config.schemaPath = "services/notification/database/schema.sql";
  return config;
}

ListenerConfig NotificationConfig::resolveListener()
{
  return ListenerConfig::resolveServiceTls("notification", 7028);
}

GrpcListenerConfig NotificationConfig::resolveRpcListener()
{
  return GrpcListenerConfig::resolve(7038);
}

NotificationIdentityConfig NotificationConfig::resolveIdentity()
{
  NotificationIdentityConfig config;
  config.target = ConfigService::getString("identity.target");
  config.rpcSecret = ConfigService::getString("identity.rpc_secret");
  return config;
}
