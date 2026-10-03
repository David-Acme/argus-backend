#include "camera-config.hxx"

#include <config/config-service.hxx>

CameraDbConfig CameraConfig::resolveDb()
{
  CameraDbConfig config;
  config.dbPath = ConfigService::getString("camera.db");
  if (config.dbPath.empty())
    config.dbPath = "database/camera.db";
  config.schemaPath = ConfigService::getString("camera.schema");
  if (config.schemaPath.empty())
    config.schemaPath = "services/camera/database/schema.sql";
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
