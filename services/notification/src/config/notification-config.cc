#include "notification-config.hxx"

#include <config/config-service.hxx>
#include <settings/settings-rpc.hxx>

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

std::vector<argus::client::CallerCredential>
NotificationConfig::resolveSettingsCallers()
{
  const std::string secret = ConfigService::getString("grpc.caller_settings");
  if (secret == ConfigService::getString("grpc.caller_guard") ||
      secret == ConfigService::getString("grpc.caller_sync"))
    return {};
  return settingsCallers({{kSettingsCaller, secret}});
}

int64_t NotificationConfig::resolveAckWindowS()
{
  constexpr int64_t kDefaultAckWindowS = 86400;
  if (!ConfigService::hasKey("notifications.ack_window_s"))
    return kDefaultAckWindowS;
  const int64_t window = ConfigService::getInt("notifications.ack_window_s");
  return window > 0 ? window : kDefaultAckWindowS;
}

int64_t NotificationConfig::resolveSelfTestIntervalS()
{
  if (!ConfigService::hasKey("notifications.selftest_interval_s"))
    return 300;
  return ConfigService::getInt("notifications.selftest_interval_s");
}
