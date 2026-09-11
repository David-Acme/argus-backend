#include <camera/camera-sync-client.hxx>
#include <config/app-config.hxx>
#include <controllers/health-controller.hxx>
#include <controllers/llm-controller.hxx>
#include <drogon/drogon.h>
#include <identity/identity-client.hxx>
#include <memory/catalog-replica.hxx>
#include <server/listener-config.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/memory/in-process-memory-chat.hxx>
#include <shared/services/memory/memory-service.hxx>
#include <shared/services/memory/sqlite-graph.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/services/sqlite/vec-db.hxx>
#include <shared/services/tools/tool-registry.hxx>
#include <shared/wrapper/blocking-task/blocking-task.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>

#include <chrono>
#include <json/value.h>
#include <llama.h>
#include <memory>
#include <string>
#include <thread>

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

CatalogReplica::Snapshot fetchCatalogSnapshot()
{
  CatalogReplica::Snapshot snapshot;

  const auto identityTarget = ConfigService::getString("identity.target");
  if (!identityTarget.empty()) {
    const IdentityClient client(
        identityTarget, ConfigService::getString("identity.rpc_secret"));
    if (const auto persons = client.listPersons()) {
      for (const auto& person : persons->persons())
        snapshot.persons.push_back({.id = person.id(),
                                    .userId = person.user_id(),
                                    .name = person.name(),
                                    .alias = person.alias()});
    }
    else {
      LOG_WARN << "argus-llm: identity snapshot read failed at "
               << identityTarget;
    }
  }

  const auto cameraTarget = ConfigService::getString("camera.grpc_target");
  if (!cameraTarget.empty()) {
    const CameraSyncClient client(cameraTarget);
    const SyncIdentity identity{
        .userId = 0, .role = "system", .device = "argus-llm"};
    if (const auto catalog = client.listCatalog(identity)) {
      for (const auto& camera : catalog->cameras())
        snapshot.cameras.push_back({.id = camera.id(), .name = camera.name()});
      for (const auto& zone : catalog->zones())
        snapshot.zones.push_back({.id = zone.id(), .name = zone.name()});
      for (const auto& stream : catalog->streams())
        snapshot.streams.push_back(
            {.id = stream.id(), .label = stream.label()});
    }
    else {
      LOG_WARN << "argus-llm: camera catalog read failed at " << cameraTarget;
    }
  }

  return snapshot;
}

bool hasCatalogRows(const CatalogReplica::Snapshot& snapshot)
{
  return !snapshot.persons.empty() || !snapshot.cameras.empty() ||
         !snapshot.zones.empty() || !snapshot.streams.empty();
}

CatalogReplica::Snapshot fetchCatalogSnapshotWithRetry()
{
  for (int attempt = 0; attempt < 20; ++attempt) {
    auto snapshot = fetchCatalogSnapshot();
    if (hasCatalogRows(snapshot))
      return snapshot;
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
  }
  LOG_WARN << "argus-llm: catalog snapshot sources empty after retries";
  return CatalogReplica::Snapshot{};
}

} // namespace

int main()
{
  ConfigService::load("config.toml");

  const ListenerConfig listener = ListenerConfig::resolve(7032);

  drogon::app().registerController(std::make_shared<HealthController>(HealthStatus{.serviceName = "argus-llm", .extras = {}}));
  const auto llm = std::make_shared<LlmController>();
  drogon::app().registerController(llm);

  drogon::app().loadConfigJson(drogonConfig(listener));

  // A whole-emitting tool loop outruns Drogon's 60 s idle default (f8-b4).
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

  // The memory package hosted in process over the same engine the chat rides.
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
  // The stack must register into the singleton the loop reads.
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

  drogon::app().registerBeginningAdvice([&memory, &replica]() {
    drogon::async_run([&memory,
                       &replica]() -> drogon::Task<void> {
      const auto snapshot = co_await BlockingTask<CatalogReplica::Snapshot>(
          [] { return fetchCatalogSnapshotWithRetry(); });
      if (replica)
        replica->seedFromSnapshot(snapshot);
      else
        CatalogReplica::seedSnapshot(
            {static_cast<SqliteGraph&>(memory.graph()), memory.resolver(),
             snapshot});
      co_return;
    });
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
