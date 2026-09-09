#include "productivity-config.hxx"

#include <shared/services/config-service/config-service.hxx>

ProductivityDbConfig ProductivityConfig::resolveDb()
{
  ProductivityDbConfig config;
  config.dbPath = ConfigService::getString("productivity.db");
  if (config.dbPath.empty())
    config.dbPath = "database/productivity.db";
  config.schemaPath = ConfigService::getString("productivity.schema");
  if (config.schemaPath.empty())
    config.schemaPath = "services/argus-productivity/database/schema.sql";
  return config;
}
