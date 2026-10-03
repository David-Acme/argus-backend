#include <app/rpc/llm-rpc-server.hxx>
#include <config/llm-config.hxx>
#include <camera/camera-sync-client.hxx>
#include <feature/llm/controllers/llm-controller.hxx>
#include <drogon/drogon.h>
#include <http/error-handler.hxx>
#include <http/health-controller.hxx>
#include <http/listener-config.hxx>
#include <identity/identity-client.hxx>
#include <llm/llm-client.hxx>
#include <feature/memory/infra/catalog-replica.hxx>
#include <feature/memory/repositories/memory-graph/memory-graph-repository.hxx>
#include <config/config-service.hxx>
#include <feature/encounter-closed/services/encounter-closed-consumer.hxx>
#include <runtime/thread-budget.hxx>
#include <feature/memory/services/memory/in-process-memory-chat.hxx>
#include <feature/memory/services/memory/memory-service.hxx>
#include <feature/memory/services/memory/sqlite-graph.hxx>
#include <sqlite/db-service.hxx>
#include <sqlite/vec-db.hxx>
#include <feature/llm/services/tools/tool-registry.hxx>
#include <runtime/blocking-task.hxx>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>

#include <chrono>
#include <ctime>
#include <json/value.h>
#include <llama.h>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

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

  const LlmIdentityConfig identityConfig = LlmConfig::resolveIdentity();
  if (!identityConfig.target.empty()) {
    const IdentityClient client(
        identityConfig.target, identityConfig.rpcSecret);
    if (const auto persons = client.listPersons()) {
      for (const auto& person : persons->persons())
        snapshot.persons.push_back({.id = person.id(),
                                    .userId = person.user_id(),
                                    .name = person.name(),
                                    .alias = person.alias()});
    }
    else {
      LOG_WARN << "argus-llm: identity snapshot read failed at "
               << identityConfig.target;
    }
  }

  const std::string cameraTarget = LlmConfig::resolveCameraTarget();
  if (!cameraTarget.empty()) {
    const CameraSyncClient client(
        {.target = cameraTarget,
         .credential = LlmConfig::resolveCameraCredential()});
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

struct EncounterSummaryInput
{
  int64_t cameraId{0};
  int64_t durationS{0};
  std::string grade;
  std::string lang;
};

std::string summarizeEncounter(const EncounterSummaryInput& input)
{
  if (input.lang == "en")
    return "Camera " + std::to_string(input.cameraId) +
           ": person observed for " + std::to_string(input.durationS) +
           " s (risk " + input.grade + ")";
  return "Cámara " + std::to_string(input.cameraId) +
         ": persona observada durante " + std::to_string(input.durationS) +
         " s (riesgo " + input.grade + ")";
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

}

int main()
{
  ConfigService::load("config.toml");

  const ListenerConfig listener = LlmConfig::resolveListener();

  drogon::app().registerController(std::make_shared<HealthController>(HealthStatus{.serviceName = "argus-llm", .extras = {}}));
  const auto llm = std::make_shared<LlmController>();
  drogon::app().registerController(llm);

  drogon::app().loadConfigJson(drogonConfig(listener));

  drogon::app().setIdleConnectionTimeout(600);

  drogon::app().setExceptionHandler(ErrorHandler::handleException);

  drogon::app().setCustomErrorHandler(
      [](drogon::HttpStatusCode code, const drogon::HttpRequestPtr&) {
        return ErrorHandler::unmatchedRoute(code);
      });

  llama_backend_init();

  llm->initEngine();
  if (!llm->isEngineLoaded()) {
    LOG_FATAL << "LLM engine failed to load — aborting startup";
    llama_backend_free();
    return 1;
  }

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
  for (auto& descriptor : memory.toolDescriptors())
    ToolRegistry::instance().registerTool(std::move(descriptor));

  const LlmRpcConfig rpcConfig = LlmConfig::resolveRpc();
  std::unique_ptr<LlmRpcServer> rpc;
  if (!rpcConfig.address.empty() && !rpcConfig.credentials.empty()) {
    rpc = std::make_unique<LlmRpcServer>(LlmRpcInput{
        .address = rpcConfig.address,
        .credentials = rpcConfig.credentials,
        .capabilities = [&llm] {
          const LlmPrefillStats stats = llm->service().lastPrefillStats();
          return argus::llm::Capabilities{
              .loaded = llm->service().isLoaded(),
              .defaultMaxTokens = llm->service().defaultMaxTokens(),
              .defaultTemperature = llm->service().defaultTemperature(),
              .contextSize = llm->service().contextSize(),
              .lastPromptTokens = stats.promptTokens,
              .lastReusedTokens = stats.reusedTokens,
              .lastDecodedTokens = stats.decodedTokens};
        },
        .chat = [&llm](const ChatRequest& request) {
          return llm->chatSync(request);
        },
        .chatStream = [&llm](const LlmStreamInput& input) {
          llm->chatStreamSync(input);
        },
        .slots = ThreadBudget::inferenceSlots()});
    LOG_INFO << "argus-llm gRPC chat listening on " << rpcConfig.address;
  }

  std::unique_ptr<NatsBus> bus;
  std::unique_ptr<CatalogReplica> replica;
  if (!ConfigService::getString("nats.url").empty()) {
    bus = std::make_unique<NatsBus>();
    if (!bus->connect())
      LOG_WARN << "argus-llm: NATS unavailable; the catalog replica attaches "
                  "when the bus reconnects";
    replica = std::make_unique<CatalogReplica>(CatalogReplica::Deps{
        .bus = *bus,
        .graph = static_cast<SqliteGraph&>(memory.graph()),
        .resolver = memory.resolver()});
  }

  std::unique_ptr<EncounterClosedConsumer> encounterConsumer;
  MemoryGraphRepository encounterRepository;
  const LlmMemoryConfig memoryConfig = LlmConfig::resolveMemory();
  if (bus && memory.isLoaded() && memoryConfig.observeCameraEvents) {
    const LlmIdentityConfig identityConfig = LlmConfig::resolveIdentity();
    if (identityConfig.target.empty()) {
      LOG_WARN << "argus-llm: camera memory enabled but [identity].target is "
                  "empty";
    }
    else {
      const IdentityClient identity(identityConfig.target,
                                    identityConfig.rpcSecret);
      int64_t ownerUserId = 0;
      std::string ownerLang = "es";
      if (const auto ids = identity.listNotifiableUsers();
          ids && !ids->empty()) {
        ownerUserId = ids->front();
        if (const auto user = identity.getUser(ownerUserId);
            user && user->has_user())
          ownerLang = user->user().lang();
      }
      if (ownerUserId > 0) {
        encounterConsumer = std::make_unique<EncounterClosedConsumer>(
            EncounterClosedConsumer::Dependencies{
                .bus = bus.get(),
                .graph = &static_cast<SqliteGraph&>(memory.graph()),
                .repository = &encounterRepository,
                .capture =
                    [&memory, ownerUserId, ownerLang](
                        const EncounterCaptureInput& capture) {
                      std::vector<int64_t> persons;
                      if (capture.personId > 0)
                        persons.push_back(capture.personId);
                      const auto now =
                          static_cast<int64_t>(std::time(nullptr));
                      return memory.observeSystemEvent(
                          {.channel = "camera",
                           .summary = summarizeEncounter(
                               {.cameraId = capture.cameraId,
                                .durationS = capture.durationS,
                                .grade = capture.grade,
                                .lang = ownerLang}),
                           .actor = "argus-guard",
                           .at = capture.closedAt > 0 ? capture.closedAt : now,
                           .userId = ownerUserId,
                           .lang = ownerLang,
                           .entitiesHint = persons});
                    }},
            EncounterClosedConsumer::Config{
                .stream = std::string(nats_subject::kGuardStream),
                .durable = "argus-llm-encounters",
                .subject = std::string(nats_subject::kGuardEncounterClosed),
                .maxDeliver = 10,
                .poisonMaxAttempts = 3,
                .ownerUserId = ownerUserId,
                .lang = ownerLang});
        LOG_INFO << "argus-llm: camera events feed memory (user "
                 << ownerUserId << ")";
      }
      else {
        LOG_WARN << "argus-llm: camera memory enabled but no notifiable user";
      }
    }
  }

  drogon::app().registerBeginningAdvice([&memory, &replica,
                                         &encounterConsumer]() {
    if (encounterConsumer)
      encounterConsumer->start();
    drogon::async_run([&memory,
                       &replica]() -> drogon::Task<void> {
      try {
        const auto snapshot = co_await BlockingTask<CatalogReplica::Snapshot>(
            [] { return fetchCatalogSnapshotWithRetry(); });
        if (replica)
          replica->seedFromSnapshot(snapshot);
        else
          CatalogReplica::seedSnapshot(
              {static_cast<SqliteGraph&>(memory.graph()), memory.resolver(),
               snapshot});
      }
      catch (const std::exception& error) {
        LOG_WARN << "argus-llm: catalog snapshot seed failed: "
                 << error.what();
      }
      catch (...) {
        LOG_WARN << "argus-llm: catalog snapshot seed failed with unknown "
                    "error";
      }
      if (replica)
        replica->subscribe();
      co_return;
    });
  });

  LOG_INFO << "argus-llm listening on " << listener.host << ":"
           << listener.port;

  drogon::app()
      .setThreadNum(0)
      .run();

  if (rpc)
    rpc->shutdown();
  if (replica)
    replica->stop();
  if (encounterConsumer)
    encounterConsumer->stop();
  memory.shutdown();
  llm->shutdownEngine();
  llama_backend_free();
  return 0;
}
