#include <app/rpc/llm-rpc-server.hxx>
#include <config/llm-config.hxx>
#include <camera/camera-sync-client.hxx>
#include <feature/memory/infra/notification-reminder-calls.hxx>
#include <feature/memory/infra/productivity-reminder-rows.hxx>
#include <feature/pending-intent/infra/notification-intent-notifier.hxx>
#include <feature/pending-intent/services/module-intent-feed.hxx>
#include <feature/pending-intent/services/pending-intent-service.hxx>
#include <feature/llm/controllers/llm-controller.hxx>
#include <feature/llm/services/tools/core-tools.hxx>
#include <feature/llm/services/tools/tool-directory.hxx>
#include <feature/llm/services/turn/model-text.hxx>
#include <auth/module-feed.hxx>
#include <auth/module-gate.hxx>
#include <mcp/client.hxx>
#include <mcp/grpc-tool-transport.hxx>
#include <mcp/local-transport.hxx>
#include <feature/settings/llm-components.hxx>
#include <feature/settings/llm-settings.hxx>
#include <settings/settings-rpc.hxx>
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
#include <runtime/log-output.hxx>
#include <runtime/shutdown-signal.hxx>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>

#include <algorithm>
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
        identityConfig.target,
        argus::client::PeerCredential{.credential = identityConfig.credential,
                                      .fleetSecret = identityConfig.rpcSecret});
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

turn::PolicySet configuredPolicies()
{
  const auto policyOf = [](const LlmDecisionConfig& decision) {
    return turn::DecisionPolicy{.act = decision.act, .ask = decision.ask, .margin = decision.margin};
  };
  turn::PolicySet policies;
  if (const auto fallback = LlmConfig::resolveDecision())
    policies.setFallback(policyOf(*fallback));
  for (const std::string_view id : turn::kDeciderIds) {
    const auto own = LlmConfig::resolveDecision(id);
    const bool witnessOnly = LlmConfig::resolveWitnessOnly(id);
    if (!own && !witnessOnly)
      continue;
    turn::DecisionPolicy policy = own ? policyOf(*own) : policies.of(id);
    policy.witnessOnly = witnessOnly;
    policies.set(std::string(id), policy);
  }
  return policies;
}

}

int main()
{
  log_output::flushEachLine();
  ConfigService::load("config.toml");

  const ListenerConfig listener = LlmConfig::resolveListener();

  drogon::app().registerController(std::make_shared<HealthController>(HealthStatus{.serviceName = "argus-llm", .extras = {}}));
  const auto llm = std::make_shared<LlmController>();
  drogon::app().registerController(llm);
  llm->adapter().flow().usePolicies(configuredPolicies());

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
  const turn::ModelText modelText(memory.extraction());
  llm->adapter().flow().useText(modelText);
  std::shared_ptr<NotificationClient> notificationClient;
  if (const LlmNotificationConfig notifications = LlmConfig::resolveNotifications();
      !notifications.target.empty() && !notifications.credential.empty()) {
    notificationClient = std::make_shared<NotificationClient>(
        NotificationClientConfig{.target = notifications.target, .credential = notifications.credential});
    memory.setReminderCalls(std::make_shared<NotificationReminderCalls>(notificationClient));
    LOG_INFO << "argus-llm: timed reminders schedule a call through "
             << notifications.target;
  }
  ToolRegistry& tools = ToolRegistry::instance();
  const argus::mcp::ClientIdentity toolClient{.name = "argus-llm", .version = "1"};
  tools.addProvider({.id = "llm",
                     .client = std::make_shared<argus::mcp::McpClient>(
                         std::make_shared<argus::mcp::LocalTransport>(coreToolServer(memory.toolDescriptors())),
                         toolClient)});
  const auto providers = LlmConfig::resolveToolProviders();
  std::shared_ptr<const ReminderRowWriter> reminderRows;
  if (const auto productivity = std::ranges::find(providers, std::string("productivity"), &LlmToolProviderConfig::id);
      productivity != providers.end()) {
    try {
      reminderRows = std::make_shared<ProductivityReminderRows>(std::make_shared<ProductivityReminderClient>(
          ReminderClientConfig{.target = productivity->target, .credential = productivity->credential}));
      memory.setReminderRows(reminderRows);
    }
    catch (const std::exception& error) {
      LOG_WARN << "argus-llm: reminders will not be listed in productivity: " << error.what();
    }
  }
  for (const auto& provider : providers) {
    try {
      tools.addProvider(
          {.id = provider.id,
           .client = std::make_shared<argus::mcp::McpClient>(
               std::make_shared<argus::mcp::GrpcToolTransport>(
                   argus::mcp::ToolEndpoint{.target = provider.target,
                                            .credential = provider.credential,
                                            .timeout = std::chrono::milliseconds(15000)}),
               toolClient)});
    }
    catch (const std::exception& error) {
      LOG_WARN << "argus-llm: tools of " << provider.id << " are not served: " << error.what();
    }
  }
  tools.refresh("llm");
  ToolDirectory toolDirectory(tools, {});
  toolDirectory.start();

  SettingsRegistry settings(llmSettingsCatalog());
  DiskComponentHost components(llmComponents(
      {.modelsDir = LlmConfig::resolveComponentsRoot(), .loaded = [&llm] { return llm->service().isLoaded(); }}));
  settings.onChange([&llm](const std::vector<std::string>&) { llm->service().refreshSampling(); });
  if (llama_supports_gpu_offload())
    settings.declareCapability("gpu");

  const LlmRpcConfig rpcConfig = LlmConfig::resolveRpc();
  if (const auto voice = std::ranges::find(rpcConfig.credentials, std::string(kIdentityCaller),
                                           &std::pair<std::string, std::string>::first);
      voice != rpcConfig.credentials.end())
    llm->setIdentityCredential(voice->second);
  else
    LOG_WARN << "argus-llm: no [rpc.callers] voice credential; the loopback HTTP leg trusts the "
                "identity its callers declare";
  std::unique_ptr<LlmRpcServer> rpc;
  std::unique_ptr<SettingsRpcService> settingsRpc;
  if (!rpcConfig.address.empty() && !rpcConfig.credentials.empty()) {
    std::vector<grpc::Service*> services;
    if (!rpcConfig.settingsCredentials.empty()) {
      settingsRpc = std::make_unique<SettingsRpcService>(SettingsRpcInput{
          .service = "llm", .registry = &settings, .credentials = rpcConfig.settingsCredentials});
      settingsRpc->attachComponents(components);
      services.push_back(settingsRpc.get());
    }
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
        .slots = ThreadBudget::inferenceSlots(),
        .services = std::move(services)});
    LOG_INFO << "argus-llm gRPC chat listening on " << rpcConfig.address;
  }

  std::shared_ptr<NatsBus> bus;
  std::unique_ptr<CatalogReplica> replica;
  if (!ConfigService::getString("nats.url").empty()) {
    bus = std::make_shared<NatsBus>();
    if (!bus->connect())
      LOG_WARN << "argus-llm: NATS unavailable; the catalog replica attaches "
                  "when the bus reconnects";
    replica = std::make_unique<CatalogReplica>(CatalogReplica::Deps{
        .bus = *bus,
        .graph = static_cast<SqliteGraph&>(memory.graph()),
        .resolver = memory.resolver()});
  }

  const auto modules = module_gate::install({.service = "llm", .bus = bus});
  moduleGate().onChange([&toolDirectory](const ModuleChange&) { toolDirectory.requestRefresh(); });

  const auto intents = std::make_shared<PendingIntentService>(
      PendingIntentDependencies{
          .graph = static_cast<SqliteGraph*>(&memory.graph()),
          .run = [&llm](const tools::ToolCall& call, UserRole role) {
            return llm->adapter().executor().execute(call, {.role = role, .modules = moduleGate().snapshot()});
          },
          .notifier = notificationClient ? std::make_shared<NotificationIntentNotifier>(notificationClient) : nullptr,
          .reminders = reminderRows,
          .moduleName = [](const ModuleLabel& label) {
            const ModuleSnapshot snapshot = moduleGate().snapshot();
            const auto found = std::ranges::find(snapshot.modules(), label.module, &ModuleFlag::id);
            if (found == snapshot.modules().end())
              return label.module;
            const std::string& name = label.lang == "en" ? found->name.en : found->name.es;
            return name.empty() ? label.module : name;
          },
          .moduleActive = [](const std::string& module) { return moduleGate().snapshot().enabled(module); },
          .clock = {}},
      PendingIntentLimits{});
  llm->adapter().executor().attachLedger(intents);
  std::unique_ptr<ModuleIntentFeed> intentFeed;
  if (bus)
    intentFeed = std::make_unique<ModuleIntentFeed>(bus, *intents, ModuleIntentFeed::defaults());

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
      const IdentityClient identity(
          identityConfig.target,
          argus::client::PeerCredential{
              .credential = identityConfig.credential,
              .fleetSecret = identityConfig.rpcSecret});
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
                                         &encounterConsumer, &intents, &intentFeed]() {
    intents->start();
    if (intentFeed)
      intentFeed->start();
    if (encounterConsumer)
      encounterConsumer->start();
    drogon::async_run([&memory,
                       &replica]() -> drogon::Task<void> {
      try {
        co_await BlockingTask<void>([&memory, &replica] {
          const auto snapshot = fetchCatalogSnapshotWithRetry();
          if (replica)
            replica->seedFromSnapshot(snapshot);
          else
            CatalogReplica::seedSnapshot({.graph = static_cast<SqliteGraph&>(memory.graph()),
                                          .resolver = memory.resolver(),
                                          .snapshot = snapshot});
        });
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

  shutdown_signal::onStop(shutdown_signal::drainOf(llm->streams(), "llm-streams"));
  shutdown_signal::onStop(shutdown_signal::drainOf(toolDirectory, "llm-tools"));
  shutdown_signal::onStop(shutdown_signal::drainOf(*intents, "llm-intents"));
  if (intentFeed)
    shutdown_signal::onStop(shutdown_signal::drainOf(*intentFeed, "llm-intent-feed"));
  if (rpc)
    shutdown_signal::onStop(shutdown_signal::drainOf(*rpc, "llm-rpc"));
  if (encounterConsumer)
    shutdown_signal::onStop(shutdown_signal::drainOf(*encounterConsumer, "llm-encounters"));
  if (replica)
    shutdown_signal::onStop(shutdown_signal::drainOf(*replica, "llm-catalog-replica"));
  shutdown_signal::onStop(shutdown_signal::drainOf(memory, "llm-memory-worker"));

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
