#include "camera-config.hxx"

#include <config/config-service.hxx>
#include <settings/settings-rpc.hxx>

#include <algorithm>
#include <array>
#include <string_view>

CameraDbConfig CameraConfig::resolveDb()
{
  CameraDbConfig config;
  config.dbPath = ConfigService::getString("camera.db");
  if (config.dbPath.empty())
    config.dbPath = "database/camera.db";
  config.schemaPath = ConfigService::getString("camera.schema");
  if (config.schemaPath.empty())
    config.schemaPath = "services/camera/database/schema.sql";
  config.secretKeyPath = ConfigService::getString("camera.secret_key");
  if (config.secretKeyPath.empty()) {
    std::string_view file = config.dbPath;
    if (file.starts_with("file:"))
      file = file.substr(5, file.find('?') == std::string_view::npos ? std::string_view::npos
                                                                     : file.find('?') - 5);
    const auto slash = file.rfind('/');
    config.secretKeyPath =
        (slash == std::string_view::npos ? std::string() : std::string(file.substr(0, slash + 1))) +
        "camera-secret.key";
  }
  return config;
}

PrivacyNoticeConfig CameraConfig::resolvePrivacyNotice()
{
  PrivacyNoticeConfig config;
  config.identityTarget = ConfigService::getString("identity.target");
  config.identityCredential = ConfigService::getString("identity.credential");
  config.identityFleetSecret = ConfigService::getString("identity.rpc_secret");
  config.notificationTarget = ConfigService::getString("notifications.target");
  config.notificationCredential = ConfigService::getString("notifications.credential");
  return config;
}

GuardDutyConfig CameraConfig::resolveGuardDuty()
{
  return {.target = ConfigService::getString("guard.duty_target"),
          .credential = ConfigService::getString("guard.duty_credential")};
}

ListenerConfig CameraConfig::resolveListener()
{
  return ListenerConfig::resolveServiceTls("camera", 7026);
}

CameraPtzConfig CameraConfig::resolvePtz()
{
  CameraPtzConfig config;
  const auto seconds = [](const std::string& key, int64_t fallbackMs) {
    const int64_t value = ConfigService::getInt(key);
    return value > 0 ? value * 1000 : fallbackMs;
  };
  config.parkMs = seconds("ptz.park_seconds", config.parkMs);
  config.idleReleaseMs = seconds("ptz.idle_release_seconds", config.idleReleaseMs);
  const int64_t settle = ConfigService::getInt("ptz.settle_ms");
  if (settle > 0)
    config.settleMs = settle;
  config.returnAfterLookMs = seconds("ptz.return_after_look_seconds", config.returnAfterLookMs);
  config.returnAfterFollowMs = seconds("ptz.return_after_follow_seconds", config.returnAfterFollowMs);
  config.calibrateEveryMs = seconds("ptz.calibrate_every_seconds", config.calibrateEveryMs);
  return config;
}

CameraWatchConfig CameraConfig::resolveWatch()
{
  CameraWatchConfig config;
  const auto positive = [](const std::string& key, int64_t fallback) {
    const int64_t value = ConfigService::getInt(key);
    return value > 0 ? value : fallback;
  };
  const auto nonNegative = [](const std::string& key, int64_t fallback) {
    if (!ConfigService::hasKey(key))
      return fallback;
    const int64_t value = ConfigService::getInt(key);
    return value >= 0 ? value : fallback;
  };
  config.micFrameMs = std::clamp<int64_t>(positive("watch.mic_frame_ms", config.micFrameMs), 10, 200);
  config.micReconnectMs = positive("watch.mic_reconnect_ms", config.micReconnectMs);
  config.micReconnectMaxMs =
      std::max(config.micReconnectMs, positive("watch.mic_reconnect_max_ms", config.micReconnectMaxMs));
  config.micLingerMs = nonNegative("watch.mic_linger_ms", config.micLingerMs);
  config.gateRecheckMs = positive("watch.gate_recheck_ms", config.gateRecheckMs);
  config.micQueueFrames = positive("watch.mic_queue_frames", config.micQueueFrames);
  config.earconMs = nonNegative("watch.earcon_ms", config.earconMs);
  config.leadSilenceMs = nonNegative("watch.lead_silence_ms", config.leadSilenceMs);
  config.tailPadMs = nonNegative("watch.tail_pad_ms", config.tailPadMs);
  config.speakQueueMs = positive("watch.speak_queue_ms", config.speakQueueMs);
  config.speakIdleMs = positive("watch.speak_idle_ms", config.speakIdleMs);
  config.packetMs = positive("watch.packet_ms", config.packetMs);
  config.maxListeners = positive("watch.max_listeners", config.maxListeners);
  config.boardRefreshMs = positive("watch.board_refresh_ms", config.boardRefreshMs);
  return config;
}

CameraHealthConfig CameraConfig::resolveHealth()
{
  CameraHealthConfig config;
  config.enabled = !ConfigService::hasKey("health.enabled") ||
                   ConfigService::getBool("health.enabled");
  config.intervalMs = ConfigService::getInt("health.interval_ms");
  if (config.intervalMs <= 0)
    config.intervalMs = 60000;
  const auto threshold = [](const std::string& key, double fallback) {
    const double value = ConfigService::getDouble(key);
    return value > 0 ? value : fallback;
  };
  config.thresholds.dark = threshold("health.dark_threshold", 25.0);
  config.thresholds.bright = threshold("health.bright_threshold", 235.0);
  config.thresholds.blur = threshold("health.blur_threshold", 18.0);
  config.thresholds.sceneDiff = threshold("health.scene_diff", 0.35);
  if (const int seconds = ConfigService::getInt("health.rebaseline_after_s");
      seconds > 0)
    config.rebaselineAfterMs = static_cast<int64_t>(seconds) * 1000;
  return config;
}

CameraOnvifConfig CameraConfig::resolveOnvif()
{
  return {.events = !ConfigService::hasKey("onvif.events") || ConfigService::getBool("onvif.events")};
}

std::string CameraConfig::resolveGuardCallerSecret()
{
  return ConfigService::getString("grpc.caller_guard");
}

std::string CameraConfig::resolveSyncCallerSecret()
{
  return ConfigService::getString("grpc.caller_sync");
}

std::string CameraConfig::resolveLlmCallerSecret()
{
  return ConfigService::getString("grpc.caller_llm");
}

std::string CameraConfig::resolveVoiceCallerSecret()
{
  return ConfigService::getString("grpc.caller_voice");
}

std::vector<argus::client::CallerCredential> CameraConfig::resolveSettingsCallers()
{
  const std::string secret = ConfigService::getString("grpc.caller_settings");
  const std::array otherCallers{resolveGuardCallerSecret(),
                                resolveSyncCallerSecret(),
                                resolveLlmCallerSecret(),
                                resolveVoiceCallerSecret()};
  if (std::ranges::find(otherCallers, secret) != otherCallers.end())
    return {};
  return settingsCallers({{kSettingsCaller, secret}});
}
