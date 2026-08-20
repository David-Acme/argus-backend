#include <drogon/drogon.h>
#include <json/value.h>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/queue/adapter/queue-manager-service-adapter.hxx>

bool QueueManagerServiceAdapter::initialize()
{
  // `JobRepository` opens its own raw SQLite handle; that must not happen
  // before drogon configures its SQLite client (sqlite3_config MULTITHREAD)
  // at app().run() start, or the call fails with a FATAL log. The queue
  // therefore boots in drogon's beginning advice, after the clients exist.
  drogon::app().registerBeginningAdvice([this]() {
    manager_.start(ConfigService::getString("database.file"));
  });
  return true;
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
