#include <config/app-config.hxx>
#include <controllers/health-controller.hxx>
#include <controllers/llm-controller.hxx>
#include <drogon/drogon.h>
#include <memory/catalog-replica.hxx>
#include <server/listener-config.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/memory/in-process-memory-chat.hxx>
#include <shared/services/memory/memory-service.hxx>
#include <shared/services/memory/sqlite-graph.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/services/sqlite/vec-db.hxx>
#include <shared/services/tools/tool-registry.hxx>
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

// Read-only snapshot source for the catalog seeds. Opened BEFORE
// loadConfigJson: the first sqlite3 client creation runs sqlite3_config
// globally (Ruling BW), so it must not race Drogon's own client setup.
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
    LOG_INFO << "argus-llm: snapshot source opened read-only (" << configKey
             << "): " << path;
    return client;
  }
  catch (const std::exception& error) {
    LOG_WARN << "argus-llm: source client " << configKey
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

  const ListenerConfig listener = ListenerConfig::resolve(7032);

  drogon::app().registerController(std::make_shared<HealthController>(HealthStatus{.serviceName = "argus-llm", .extras = {}}));
  const auto llm = std::make_shared<LlmController>();
  drogon::app().registerController(llm);

  drogon::app().loadConfigJson(drogonConfig(listener));

  // LLM turns are silent for their whole generation, and a whole-emitting
  // tool loop can run minutes on CPU. Drogon's 60 s default idle-connection
  // timeout marks such a connection mid-request and the finished response
  // is dropped (measured: empty reply at turn end); 10 min covers a
  // 3-hop turn on the slowest box while still reaping dead connections.
  drogon::app().setIdleConnectionTimeout(600);

  drogon::app().setExceptionHandler(AppConfig::handleException);

  drogon::app().setCustomErrorHandler(
      [](drogon::HttpStatusCode code, const drogon::HttpRequestPtr&) {
        if (code == drogon::k405MethodNotAllowed)
          return AppConfig::get405Response();
        return AppConfig::get404Response();
      });

  llama_backend_init();

  llm->initEngine();
  if (!llm->isEngineLoaded()) {
    LOG_FATAL << "LLM engine failed to load — aborting startup";
    llama_backend_free();
    return 1;
  }

  // The brain: the memory package hosted in process over the same engine
  // the chat rides. One LlmService, no wire hop between loop and worker.
  InProcessMemoryChat chat(llm->service());
  MemoryService memory(VecDb::instance(), chat);
  memory.init({.deferStore = true});
  if (!memory.isLoaded()) {
    LOG_FATAL << "Memory stack failed to load — aborting startup";
    memory.shutdown();
    llm->shutdownEngine();
    llama_backend_free();
    return 1;
  }
  // One registry: the loop reads the singleton, so the stack must register
  // into it, not into a private copy.
  memory.registerTools(ToolRegistry::instance());

  std::unique_ptr<NatsBus> bus;
  std::unique_ptr<CatalogReplica> replica;
  if (!ConfigService::getString("nats.url").empty()) {
    bus = std::make_unique<NatsBus>();
    if (bus->connect()) {
      replica = std::make_unique<CatalogReplica>(CatalogReplica::Deps{
          .bus = *bus,
          .graph = static_cast<SqliteGraph&>(memory.graph()),
          .resolver = memory.resolver()});
      replica->subscribe();
    }
    else {
      LOG_WARN << "argus-llm: NATS unavailable; catalog replicas replay "
                  "changes only after a reconnect";
    }
  }

  // The stack's own openStore advice (registered inside init) runs first,
  // so the seed reads an open graph.
  drogon::app().registerBeginningAdvice(
      [&memory, &replica, identity = identityDb.get(),
       camera = cameraDb.get()]() {
        if (replica)
          replica->seedFromSnapshot(identity, camera);
        else
          CatalogReplica::seedSnapshot(
              {static_cast<SqliteGraph&>(memory.graph()),
               memory.resolver(), identity, camera});
      });

  LOG_INFO << "argus-llm listening on " << listener.host << ":"
           << listener.port;

  drogon::app()
      .setThreadNum(0)
      .run();

  memory.shutdown();
  llm->shutdownEngine();
  llama_backend_free();
  return 0;
}
