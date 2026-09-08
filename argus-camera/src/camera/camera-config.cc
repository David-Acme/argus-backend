#include "camera-config.hxx"

#include <shared/services/config-service/config-service.hxx>

CameraDbConfig CameraConfig::resolveDb()
{
  CameraDbConfig config;
  config.dbPath = ConfigService::getString("camera.db");
  if (config.dbPath.empty())
    config.dbPath = "database/camera.db";
  config.schemaPath = ConfigService::getString("camera.schema");
  if (config.schemaPath.empty())
    config.schemaPath = "argus-camera/database/schema.sql";
  return config;
}
