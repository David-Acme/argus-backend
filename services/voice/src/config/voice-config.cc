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
  return GrpcListenerConfig::resolve();
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

VoiceCameraConfig VoiceConfig::resolveCamera()
{
  return {.target = ConfigService::getString("camera.grpc_target"),
          .credential = ConfigService::getString("camera.credential")};
}

namespace
{

std::chrono::milliseconds positiveMs(const std::string& key, std::chrono::milliseconds fallback)
{
  if (!ConfigService::hasKey(key))
    return fallback;
  const int value = ConfigService::getInt(key);
  return value > 0 ? std::chrono::milliseconds(value) : fallback;
}

float decimalOf(const std::string& key, float fallback)
{
  if (!ConfigService::hasKey(key))
    return fallback;
  return static_cast<float>(ConfigService::getDouble(key));
}

int positiveInt(const std::string& key, int fallback)
{
  if (!ConfigService::hasKey(key))
    return fallback;
  const int value = ConfigService::getInt(key);
  return value > 0 ? value : fallback;
}

}

VoiceWatchConfig VoiceConfig::resolveWatch()
{
  VoiceWatchConfig config;
  config.enabled = !ConfigService::hasKey("watch.enabled") || ConfigService::getBool("watch.enabled");
  config.listRefresh = positiveMs("watch.list_refresh_ms", config.listRefresh);
  config.reconnect = positiveMs("watch.reconnect_ms", config.reconnect);
  if (const std::string stage = ConfigService::getString("watch.stage1"); !stage.empty()) {
    if (const auto parsed = wakeStageOneFromString(stage))
      config.stageOne = *parsed;
    else
      LOG_WARN << "Voice: unknown watch.stage1 '" << stage << "'; using auto";
  }
  config.ring = positiveMs("watch.ring_ms", config.ring);
  config.postRoll = positiveMs("watch.post_roll_ms", config.postRoll);
  config.wakeMaxDistance = std::clamp(positiveInt("watch.wake_max_distance", config.wakeMaxDistance), 1, 3);
  config.sttStageOneMax = positiveMs("watch.stt_stage1_max_ms", config.sttStageOneMax);
  config.sttStageOneMin = positiveMs("watch.stt_stage1_min_ms", config.sttStageOneMin);
  config.sttStageOneMinSnrDb = decimalOf("watch.stt_stage1_min_snr_db", config.sttStageOneMinSnrDb);
  config.kwsHangover = positiveMs("watch.kws_hangover_ms", config.kwsHangover);
  config.minWakeSnrDb = decimalOf("watch.min_wake_snr_db", config.minWakeSnrDb);
  config.broadcastWindow = positiveMs("watch.broadcast_window_ms", config.broadcastWindow);
  config.broadcastMinCameras = std::max(2, positiveInt("watch.broadcast_min_cameras", config.broadcastMinCameras));
  config.broadcastSpreadDb = decimalOf("watch.broadcast_spread_db", config.broadcastSpreadDb);
  config.tvRaiseDb = decimalOf("watch.tv_raise_db", config.tvRaiseDb);
  config.echoTail = positiveMs("watch.echo_tail_ms", config.echoTail);
  config.echoCorrelation = std::clamp(decimalOf("watch.echo_correlation", config.echoCorrelation), 0.1F, 1.0F);
  config.echoRaiseDb = decimalOf("watch.echo_raise_db", config.echoRaiseDb);
  config.echoMaxLag = positiveMs("watch.echo_max_lag_ms", config.echoMaxLag);
  config.floorRiseDbPerSecond = decimalOf("watch.floor_rise_db_per_s", config.floorRiseDbPerSecond);
  config.floorFallDbPerSecond = decimalOf("watch.floor_fall_db_per_s", config.floorFallDbPerSecond);
  config.fireLogSize = positiveInt("watch.fire_log_size", config.fireLogSize);
  config.fireDebounce = positiveMs("watch.fire_debounce_ms", config.fireDebounce);
  config.kwsRetry = positiveMs("watch.kws_retry_ms", config.kwsRetry);
  config.arbitrationWindow = std::max(config.broadcastWindow,
                                      positiveMs("watch.arbitration_window_ms", config.arbitrationWindow));
  config.stageTwoCandidates = std::clamp(positiveInt("watch.stage2_candidates", config.stageTwoCandidates), 1, 4);
  config.weightWake = decimalOf("watch.weight_wake", config.weightWake);
  config.weightSnr = decimalOf("watch.weight_snr", config.weightSnr);
  config.weightStageTwo = decimalOf("watch.weight_stage2", config.weightStageTwo);
  config.gainWeight = std::clamp(decimalOf("watch.gain_weight", config.gainWeight), 0.0F, 1.0F);
  config.gainAlpha = std::clamp(decimalOf("watch.gain_alpha", config.gainAlpha), 0.01F, 1.0F);
  return config;
}

VoiceGuardConfig VoiceConfig::resolveGuard()
{
  return {.target = ConfigService::getString("guard.presence_target"),
          .credential = ConfigService::getString("guard.presence_credential")};
}
