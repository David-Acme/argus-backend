#include "notification-config.hxx"

#include <shared/services/config-service/config-service.hxx>

NotificationDbConfig NotificationConfig::resolveDb()
{
  NotificationDbConfig config;
  config.dbPath = ConfigService::getString("notifications.db");
  if (config.dbPath.empty())
    config.dbPath = "database/notification.db";
  config.schemaPath = ConfigService::getString("notifications.schema");
  if (config.schemaPath.empty())
    config.schemaPath = "database/notification-schema.sql";
  return config;
}
