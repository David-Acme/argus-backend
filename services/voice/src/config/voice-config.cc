#include "voice-config.hxx"

#include <config/config-service.hxx>

#include <algorithm>
#include <settings/settings-rpc.hxx>
#include <trantor/utils/Logger.h>

ListenerConfig VoiceConfig::resolveHealthListener()
{
  return ListenerConfig::resolve(7035, "server.health_port");
}

GrpcListenerConfig VoiceConfig::resolveGrpcListener()
{
  return GrpcListenerConfig::resolve(7034);
}

std::string VoiceConfig::resolveSyncCallerSecret()
{
  return ConfigService::getString("grpc.caller_sync");
}

std::vector<argus::client::CallerCredential> VoiceConfig::resolveSettingsCallers()
{
  const std::string secret = ConfigService::getString("grpc.caller_settings");
  if (secret == resolveSyncCallerSecret())
    return {};
  return settingsCallers({{kSettingsCaller, secret}});
}

std::string VoiceConfig::resolveNotificationCallerSecret()
{
  const std::string secret = ConfigService::getString("grpc.caller_notification");
  return secret == resolveSyncCallerSecret() ? std::string() : secret;
}

VoiceRtcConfig VoiceConfig::resolveRtc()
{
  VoiceRtcConfig config;
  config.url = ConfigService::getString("rtc.url");
  config.enabled = ConfigService::hasKey("rtc.enabled") && ConfigService::getBool("rtc.enabled") &&
                   !config.url.empty();
  if (ConfigService::hasKey("rtc.rejoin_grace_ms"))
    config.rejoinGrace =
        std::chrono::milliseconds(std::max(1000, ConfigService::getInt("rtc.rejoin_grace_ms")));
  if (ConfigService::hasKey("rtc.first_join_wait_ms"))
    config.firstJoinWait =
        std::chrono::milliseconds(std::max(5000, ConfigService::getInt("rtc.first_join_wait_ms")));
  return config;
}

VoiceOpening VoiceConfig::resolveOpening()
{
  const std::string name = ConfigService::getString("voice.opening");
  if (name.empty())
    return VoiceOpening::None;
  if (const auto opening = voiceOpeningFromString(name))
    return *opening;
  LOG_WARN << "Voice: unknown voice.opening '" << name << "'; using none";
  return VoiceOpening::None;
}

bool VoiceConfig::resolveLatencyTrace()
{
  return ConfigService::hasKey("voice.trace_latency") && ConfigService::getBool("voice.trace_latency");
}

VoiceNotificationConfig VoiceConfig::resolveNotification()
{
  return {.target = ConfigService::getString("notification.target"),
          .credential = ConfigService::getString("notification.credential")};
}
