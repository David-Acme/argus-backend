#include <config/app-config.hxx>
#include <controllers/health-controller.hxx>
#include <controllers/memory-controller.hxx>
#include <drogon/drogon.h>
#include <memory/catalog-replica.hxx>
#include <server/listener-config.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/memory/sqlite-graph.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>

#include <json/value.h>
#include <llama.h>
#include <memory>
#include <string>

namespace
{
Json::Value drogonConfig(const ListenerConfig& listener)
{
  Json::Value config = ConfigService::drogonConfig();
  if (config.isNull())
    config = Json::Value(Json::objectValue);
  config["listeners"] = listenerJson(listener);
  return config;
}

std::shared_ptr<drogon::orm::DbClient>
openReadOnlySource(const char* configKey)
{
  const std::string path = ConfigService::getString(configKey);
  if (path.empty())
    return nullptr;
  DbService::enableUriFilenames();
  try {
    const auto client = drogon::orm::DbClient::newSqlite3Client(
        "filename=file:" + path + "?mode=ro", 1);
    client->execSqlSync("PRAGMA busy_timeout = 5000");
    LOG_INFO << "argus-memory: snapshot source opened read-only (" << configKey
             << "): " << path;
    return client;
  }
  catch (const std::exception& error) {
    LOG_WARN << "argus-memory: source client " << configKey
             << " unavailable (" << error.what() << ")";
    return nullptr;
  }
}

} // namespace

int main()
{
  ConfigService::load("config.toml");

  const auto identityDb = openReadOnlySource("identity.db");
  const auto cameraDb = openReadOnlySource("camera.db");

  const ListenerConfig listener = ListenerConfig::resolve(7033);

  drogon::app().registerController(std::make_shared<HealthController>(HealthStatus{.serviceName = "argus-memory", .extras = {}}));
  const auto memory = std::make_shared<MemoryController>();
  drogon::app().registerController(memory);

  drogon::app().loadConfigJson(drogonConfig(listener));

  drogon::app().setExceptionHandler(AppConfig::handleException);

  drogon::app().setCustomErrorHandler(
      [](drogon::HttpStatusCode code, const drogon::HttpRequestPtr&) {
        if (code == drogon::k405MethodNotAllowed)
          return AppConfig::get405Response();
        return AppConfig::get404Response();
      });

  llama_backend_init();

  memory->initStack();
  if (!memory->isStackLoaded()) {
    LOG_FATAL << "Memory stack failed to load — aborting startup";
    llama_backend_free();
    return 1;
  }

  std::unique_ptr<NatsBus> bus;
  std::unique_ptr<CatalogReplica> replica;
  if (!ConfigService::getString("nats.url").empty()) {
    bus = std::make_unique<NatsBus>();
    if (bus->connect()) {
      replica = std::make_unique<CatalogReplica>(CatalogReplica::Deps{
          .bus = *bus,
          .graph = static_cast<SqliteGraph&>(memory->service().graph()),
          .resolver = memory->service().resolver()});
      replica->subscribe();
    }
    else {
      LOG_WARN << "argus-memory: NATS unavailable; catalog replicas replay "
                  "changes only after a reconnect";
    }
  }

  drogon::app().registerBeginningAdvice(
      [memory, &replica, identity = identityDb.get(),
       camera = cameraDb.get()]() {
        if (replica)
          replica->seedFromSnapshot(identity, camera);
        else
          CatalogReplica::seedSnapshot(
              {static_cast<SqliteGraph&>(memory->service().graph()),
               memory->service().resolver(), identity, camera});
      });

  LOG_INFO << "argus-memory listening on " << listener.host << ":"
           << listener.port;

  drogon::app()
      .setThreadNum(0)
      .run();

  memory->shutdownStack();
  llama_backend_free();
  return 0;
}
