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
  if (ConfigService::hasKey("rtc.max_concurrent_calls"))
    config.maxConcurrentCalls =
        std::clamp(ConfigService::getInt("rtc.max_concurrent_calls"), 1, 8);
  constexpr std::size_t kMinSecretBytes = 32;
  config.enabled = ConfigService::hasKey("rtc.enabled") && ConfigService::getBool("rtc.enabled") &&
                   !config.apiKey.empty() && config.apiSecret.size() >= kMinSecretBytes &&
                   config.apiKey.find("CHANGE_ME") == std::string::npos &&
                   config.apiSecret.find("CHANGE_ME") == std::string::npos &&
                   !config.serverUrl.empty();
  return config;
}

namespace
{
struct BoundedSecondsInput
{
  const char* key;
  int64_t fallback;
  int64_t min;
  int64_t max;
};

int64_t boundedSeconds(const BoundedSecondsInput& input)
{
  if (!ConfigService::hasKey(input.key))
    return input.fallback;
  return std::clamp<int64_t>(ConfigService::getInt(input.key), input.min,
                             input.max);
}
}

SyncHeartbeatConfig SyncConfig::resolveHeartbeat()
{
  constexpr int64_t kMinute = 60;
  return {.intervalSeconds = boundedSeconds(
              {.key = "heartbeat.interval_seconds", .fallback = 60, .min = 15, .max = 300}),
          .graceSeconds = kMinute * boundedSeconds({.key = "heartbeat.grace_minutes",
                                                    .fallback = 45,
                                                    .min = 15,
                                                    .max = 720}),
          .socketGraceSeconds = boundedSeconds({.key = "heartbeat.socket_grace_seconds",
                                                .fallback = 180,
                                                .min = 60,
                                                .max = 3600}),
          .guardStaleSeconds = boundedSeconds({.key = "heartbeat.guard_stale_seconds",
                                               .fallback = 90,
                                               .min = 30,
                                               .max = 3600}),
          .pushIntervalSeconds = boundedSeconds({.key = "heartbeat.push_interval_seconds",
                                                 .fallback = 900,
                                                 .min = 300,
                                                 .max = 3600}),
          .refillSeconds = boundedSeconds({.key = "heartbeat.refill_seconds",
                                           .fallback = 300,
                                           .min = 30,
                                           .max = 3600}),
          .presenceTarget = ConfigService::getString("guard.presence_target"),
          .presenceCredential = ConfigService::getString("guard.presence_credential")};
}
