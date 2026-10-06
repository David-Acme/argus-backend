#include "productivity-config.hxx"

#include <config/config-service.hxx>

ProductivityDbConfig ProductivityConfig::resolveDb()
{
  ProductivityDbConfig config;
  config.dbPath = ConfigService::getString("productivity.db");
  if (config.dbPath.empty())
    config.dbPath = "database/productivity.db";
  config.schemaPath = ConfigService::getString("productivity.schema");
  if (config.schemaPath.empty())
    config.schemaPath = "services/productivity/database/schema.sql";
  return config;
}

ListenerConfig ProductivityConfig::resolveListener()
{
  return ListenerConfig::resolveServiceTls("productivity", 7027);
}

ProductivityNotificationConfig ProductivityConfig::resolveNotifications()
{
  return {.target = ConfigService::getString("notifications.target"),
          .credential = ConfigService::getString("notifications.credential")};
}

ProductivityAgendaConfig ProductivityConfig::resolveAgenda()
{
  ProductivityAgendaConfig config;
  if (ConfigService::hasKey("agenda.enabled"))
    config.enabled = ConfigService::getBool("agenda.enabled");
  return config;
}

std::string ProductivityConfig::resolveSettingsCredential()
{
  std::string secret = ConfigService::getString("grpc.caller_settings");
  if (!secret.empty() && secret == ConfigService::getString("grpc.caller_sync"))
    return {};
  return secret;
}
