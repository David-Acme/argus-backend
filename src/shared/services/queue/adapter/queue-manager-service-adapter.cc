#include <json/value.h>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/queue/adapter/queue-manager-service-adapter.hxx>

bool QueueManagerServiceAdapter::initialize()
{
  return manager_.start(ConfigService::getString("database.file"));
}

bool QueueManagerServiceAdapter::isLoaded() const
{
  return true;
}

void QueueManagerServiceAdapter::shutdown()
{
  manager_.shutdown();
}

Json::Value QueueManagerServiceAdapter::health() const
{
  Json::Value root;
  root["queues"] = Json::Value(Json::arrayValue);
  return root;
}
