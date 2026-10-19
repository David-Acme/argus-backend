#include "camera-config.hxx"

#include <config/config-service.hxx>
#include <settings/settings-rpc.hxx>

#include <algorithm>
#include <array>
#include <string_view>
#include <type_traits>

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
  const auto positive = [](const std::string& key, auto fallback) {
    using Value = decltype(fallback);
    if constexpr (std::is_floating_point_v<Value>) {
      const double value = ConfigService::getDouble(key);
      return value > 0 ? static_cast<Value>(value) : fallback;
    }
    else {
      const int value = ConfigService::getInt(key);
      return value > 0 ? static_cast<Value>(value) : fallback;
    }
  };
  config.dailyMoveCap = positive("ptz.daily_moves", config.dailyMoveCap);
  config.movesPerMinute = positive("ptz.moves_per_minute", config.movesPerMinute);
  config.deadZone = positive("ptz.dead_zone", config.deadZone);
  config.fovHorizontalDeg = positive("ptz.fov_h_deg", config.fovHorizontalDeg);
  config.fovVerticalDeg = positive("ptz.fov_v_deg", config.fovVerticalDeg);
  config.lookBudgetMs = positive("ptz.look_budget_ms", config.lookBudgetMs);
  config.followLostMs = seconds("ptz.follow_lost_seconds", config.followLostMs);
  config.leadMinSamples = positive("ptz.lead_min_samples", config.leadMinSamples);
  config.leadConsistency = positive("ptz.lead_consistency", config.leadConsistency);
  config.patrolEnabled = !ConfigService::hasKey("ptz.patrol_enabled") || ConfigService::getBool("ptz.patrol_enabled");
  config.patrolEveryMs = seconds("ptz.patrol_every_seconds", config.patrolEveryMs);
  config.patrolDwellMs = seconds("ptz.patrol_dwell_seconds", config.patrolDwellMs);
  return config;
}

CameraLookConfig CameraConfig::resolveLook()
{
  CameraLookConfig config;
  config.enabled = !ConfigService::hasKey("look.enabled") || ConfigService::getBool("look.enabled");
  const auto decimal = [](const std::string& key, double fallback) {
    const double value = ConfigService::getDouble(key);
    return value > 0 ? value : fallback;
  };
  const auto integer = [](const std::string& key, int64_t fallback) {
    const int value = ConfigService::getInt(key);
    return value > 0 ? static_cast<int64_t>(value) : fallback;
  };
  config.doubtMinConfidence = decimal("look.doubt_min_conf", config.doubtMinConfidence);
  config.smallBoxShare = decimal("look.small_box_share", config.smallBoxShare);
  config.grabs = static_cast<int>(integer("look.grabs", config.grabs));
  config.budgetMs = integer("look.budget_ms", config.budgetMs);
  config.cropScale = decimal("look.crop_scale", config.cropScale);
  config.confirmConfidence = decimal("look.confirm_conf", config.confirmConfidence);
  config.cooldownMs = integer("look.cooldown_s", config.cooldownMs / 1000) * 1000;
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
  const HealthThresholds defaults;
  config.thresholds.saturatedShare = threshold("tamper.saturated_share", defaults.saturatedShare);
  config.thresholds.whiteoutEdgeRatio = threshold("tamper.whiteout_edge_ratio", defaults.whiteoutEdgeRatio);
  config.thresholds.blindEdgeRatio = threshold("tamper.blind_edge_ratio", defaults.blindEdgeRatio);
  config.thresholds.coverEntropy = threshold("tamper.cover_entropy", defaults.coverEntropy);
  config.thresholds.coverEdgeRatio = threshold("tamper.cover_edge_ratio", defaults.coverEdgeRatio);
  config.thresholds.partialBlockShare = threshold("tamper.partial_block_share", defaults.partialBlockShare);
  config.thresholds.defocusSharpness = threshold("tamper.defocus_sharpness", defaults.defocusSharpness);
  config.thresholds.reaimShiftPx = threshold("tamper.reaim_shift_px", defaults.reaimShiftPx);
  config.thresholds.infraredChroma = threshold("tamper.infrared_chroma", defaults.infraredChroma);
  if (const int64_t interval = ConfigService::getInt("tamper.protect_interval_ms"); interval > 0)
    config.protectIntervalMs = interval;
  if (const int64_t hold = ConfigService::getInt("tamper.day_night_hold_s"); hold > 0)
    config.dayNightHoldMs = hold * 1000;
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

std::vector<argus::client::CallerCredential> CameraConfig::resolveSettingsCallers()
{
  const std::string secret = ConfigService::getString("grpc.caller_settings");
  const std::array otherCallers{resolveGuardCallerSecret(),
                                resolveSyncCallerSecret(),
                                resolveLlmCallerSecret()};
  if (std::ranges::find(otherCallers, secret) != otherCallers.end())
    return {};
  return settingsCallers({{kSettingsCaller, secret}});
}
