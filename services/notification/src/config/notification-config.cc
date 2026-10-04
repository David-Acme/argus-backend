#include "notification-config.hxx"

#include <config/config-service.hxx>
#include <settings/settings-rpc.hxx>

#include <algorithm>

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
      secret == ConfigService::getString("grpc.caller_sync") ||
      secret == ConfigService::getString("grpc.caller_voice") ||
      secret == ConfigService::getString("grpc.caller_llm") ||
      secret == ConfigService::getString("grpc.caller_productivity"))
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

NotificationSyncControlConfig NotificationConfig::resolveSyncControl()
{
  return {.target = ConfigService::getString("sync.control_target"),
          .secret = ConfigService::getString("sync.control_secret")};
}

NotificationVoiceConfig NotificationConfig::resolveVoice()
{
  return {.target = ConfigService::getString("voice.target"),
          .credential = ConfigService::getString("voice.credential")};
}

CallEngineConfig NotificationConfig::resolveCalls()
{
  CallEngineConfig config;
  const auto positive = [](const char* key, int64_t fallback) {
    if (!ConfigService::hasKey(key))
      return fallback;
    const int64_t value = ConfigService::getInt(key);
    return value > 0 ? value : fallback;
  };
  if (ConfigService::hasKey("calls.enabled"))
    config.enabled = ConfigService::getBool("calls.enabled");
  config.ringTimeoutS = positive("calls.ring_timeout_s", config.ringTimeoutS);
  if (ConfigService::hasKey("calls.in_app_grace_s"))
    config.inAppGraceS =
        std::max<int64_t>(0, ConfigService::getInt("calls.in_app_grace_s"));
  config.callGapS = positive("calls.call_gap_s", config.callGapS);
  config.maxCallsPerHour = static_cast<int>(
      positive("calls.max_calls_per_hour", config.maxCallsPerHour));
  config.arrivalAbsenceS =
      positive("calls.arrival_absence_s", config.arrivalAbsenceS);
  config.scheduledLateS = positive("calls.scheduled_late_s", config.scheduledLateS);
  const std::string lang = ConfigService::getString("notifications.lang");
  if (lang == "en" || lang == "es")
    config.fallbackLang = lang;
  return config;
}

NotificationCallCallers NotificationConfig::resolveCallCallers()
{
  NotificationCallCallers callers;
  const std::string sync = ConfigService::getString("grpc.caller_sync");
  const std::string voice = ConfigService::getString("grpc.caller_voice");
  const std::string llm = ConfigService::getString("grpc.caller_llm");
  if (!sync.empty())
    callers.answer.push_back({.service = "argus-sync", .secret = sync});
  if (!voice.empty() && voice != sync)
    callers.answer.push_back({.service = "argus-voice", .secret = voice});
  if (!llm.empty() && llm != sync && llm != voice)
    callers.schedule.push_back({.service = "argus-llm", .secret = llm});
  return callers;
}
