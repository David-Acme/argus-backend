#include "sync-config.hxx"

#include <algorithm>

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

SyncRtcConfig SyncConfig::resolveRtc()
{
  SyncRtcConfig config;
  config.apiKey = ConfigService::getString("rtc.api_key");
  config.apiSecret = ConfigService::getString("rtc.api_secret");
  config.serverUrl = ConfigService::getString("rtc.server_url");
  config.publicUrl = ConfigService::getString("rtc.public_url");
  if (ConfigService::hasKey("rtc.public_port")) {
    const int port = ConfigService::getInt("rtc.public_port");
    if (port > 0 && port <= 65535)
      config.publicPort = static_cast<uint16_t>(port);
  }
  if (ConfigService::hasKey("rtc.token_ttl_seconds"))
    config.tokenTtl = std::chrono::seconds(
        std::clamp(ConfigService::getInt("rtc.token_ttl_seconds"), 60, 3600));
  constexpr std::size_t kMinSecretBytes = 32;
  config.enabled = ConfigService::hasKey("rtc.enabled") && ConfigService::getBool("rtc.enabled") &&
                   !config.apiKey.empty() && config.apiSecret.size() >= kMinSecretBytes &&
                   config.apiKey.find("CHANGE_ME") == std::string::npos &&
                   config.apiSecret.find("CHANGE_ME") == std::string::npos &&
                   !config.serverUrl.empty();
  return config;
}
