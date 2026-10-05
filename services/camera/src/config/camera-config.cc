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

ListenerConfig CameraConfig::resolveListener()
{
  return ListenerConfig::resolveServiceTls("camera", 7026);
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
