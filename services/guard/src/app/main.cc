#include <app/rpc/presence-rpc-service.hxx>
#include <camera/camera-action-client.hxx>
#include <config/guard-config.hxx>
#include <drogon/drogon.h>
#include <feature/guard/controllers/guard-controller.hxx>
#include <feature/guard/controllers/response-controller.hxx>
#include <feature/guard/infra/identity-response-directory.hxx>
#include <feature/guard/services/response-verdict-feed.hxx>
#include <feature/guard/guard-assessment.hxx>
#include <feature/guard/guard-repository.hxx>
#include <feature/guard/guard-schedule.hxx>
#include <feature/guard/guard-schema.hxx>
#include <feature/guard/guard-service.hxx>
#include <feature/presence/controllers/presence-controller.hxx>
#include <feature/presence/infra/identity-presence-directory.hxx>
#include <feature/presence/infra/nats-presence-publisher.hxx>
#include <feature/presence/services/presence-service.hxx>
#include <feature/settings/guard-settings.hxx>
#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/role-filter.hxx>
#include <auth/valid-json-filter.hxx>
#include <http/certificate-reload.hxx>
#include <http/error-handler.hxx>
#include <http/listener-config.hxx>
#include <http/route-announcements.hxx>
#include <identity/identity-client.hxx>
#include <mdns/mdns-service.hxx>
#include <notification/notification-client.hxx>
#include <config/config-service.hxx>
#include <llm/llm-remote.hxx>
#include <sqlite/db-service.hxx>
#include <vlm/vlm-remote.hxx>
#include <nats/nats-bus.hxx>
#include <runtime/shutdown-signal.hxx>
#include <runtime/log-output.hxx>
#include <settings/settings-rpc.hxx>
#include <unistd.h>

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

namespace
{

struct GuardDrogonConfig
{
  std::string dbPath;
  ListenerConfig listener;
};

Json::Value drogonConfig(const GuardDrogonConfig& input)
{
  Json::Value config = ConfigService::drogonConfig();
  if (config.isNull())
    config = Json::Value(Json::objectValue);

  Json::Value client(Json::objectValue);
  client["name"] = "default";
  client["rdbms"] = "sqlite3";
  client["filename"] = input.dbPath;
  client["is_fast"] = false;
  client["number_of_connections"] = 1;
  client["timeout"] = -1.0;
  Json::Value clients(Json::arrayValue);
  clients.append(client);
  config["db_clients"] = clients;

  config["listeners"] = listenerJson(input.listener);
  return config;
}

struct RpcListener
{
  std::unique_ptr<SettingsRpcService> settings;
  std::unique_ptr<PresenceRpcService> presence;
  std::unique_ptr<grpc::Server> server;
};

struct RpcListenerInput
{
  SettingsRegistry& registry;
  const PresenceService& presence;
};

RpcListener startRpcListener(const RpcListenerInput& input)
{
  GuardRpcConfig rpc = GuardConfig::resolveRpc();
  if (rpc.address.empty())
    return {};
  RpcListener listener;
  grpc::ServerBuilder builder;
  if (rpc.settingsCredentials.empty()) {
    LOG_WARN << "Guard settings RPC not served: [rpc.callers] settings is "
                "empty";
  }
  else {
    listener.settings = std::make_unique<SettingsRpcService>(
        SettingsRpcInput{.service = "guard",
                         .registry = &input.registry,
                         .credentials = std::move(rpc.settingsCredentials)});
    builder.RegisterService(listener.settings.get());
  }
  if (rpc.presenceCredentials.empty()) {
    LOG_INFO << "Guard presence RPC not served: no [rpc.callers] sync or "
                "notification";
  }
  else {
    listener.presence = std::make_unique<PresenceRpcService>(
        PresenceRpcInput{.presence = &input.presence,
                         .credentials = std::move(rpc.presenceCredentials)});
    builder.RegisterService(listener.presence.get());
  }
  if (!listener.settings && !listener.presence)
    return {};
  builder.AddListeningPort(rpc.address, grpc::InsecureServerCredentials());
  listener.server = builder.BuildAndStart();
  if (!listener.server) {
    LOG_FATAL << "Guard RPC listener failed to listen on " << rpc.address;
    _exit(1);
  }
  LOG_INFO << "Guard RPC listener on " << rpc.address;
  return listener;
}

void stopRpcListener(const RpcListener& listener)
{
  if (listener.server)
    listener.server->Shutdown(std::chrono::system_clock::now() +
                              std::chrono::milliseconds(500));
}

void registerHealth()
{
  drogon::app().registerHandler(
      "/health",
      [](const drogon::HttpRequestPtr&,
         std::function<void(const drogon::HttpResponsePtr&)>&& callback) {
        Json::Value body;
        body["status"] = "ok";
        body["service"] = "argus-guard";
        callback(drogon::HttpResponse::newHttpJsonResponse(body));
      },
      {drogon::Get});
}

}

int main()
{
  log_output::flushEachLine();
  DbService::enableUriFilenames();

  ConfigService::load("config.toml");

  const GuardDbConfig db = GuardConfig::resolveDb();
  const ListenerConfig listener = GuardConfig::resolveListener();

  const GuardPeerConfig notificationsPeer = GuardConfig::resolveNotifications();
  std::unique_ptr<NotificationClient> notifications;
  if (!notificationsPeer.target.empty())
    notifications = std::make_unique<NotificationClient>(
        NotificationClientConfig{.target = notificationsPeer.target,
                                 .credential = notificationsPeer.secret});

  const GuardPeerConfig identityPeer = GuardConfig::resolveIdentity();
  std::unique_ptr<IdentityClient> identity;
  if (!identityPeer.target.empty())
    identity = std::make_unique<IdentityClient>(identityPeer.target,
                                                identityPeer.secret);

  const GuardPeerConfig actionsPeer = GuardConfig::resolveActions();
  std::unique_ptr<CameraActionClient> actions;
  if (!actionsPeer.target.empty())
    actions = std::make_unique<CameraActionClient>(
        CameraActionClientConfig{.target = actionsPeer.target,
                                 .credential = actionsPeer.secret});

  const GuardAssessEndpoints endpoints = GuardConfig::resolveAssessEndpoints();
  std::unique_ptr<VlmClient> vlm;
  if (!endpoints.vlmUrl.empty() || !endpoints.vlmTarget.empty())
    vlm = std::make_unique<VlmClient>(
        endpoints.vlmUrl, static_cast<double>(endpoints.timeoutMs) / 1000.0);

  std::unique_ptr<LlmClient> llm;
  if (!endpoints.llmUrl.empty() || !endpoints.llmTarget.empty())
    llm = std::make_unique<LlmClient>(endpoints.llmUrl, endpoints.timeoutMs);

  GuardAssessment assessment(
      {.camera = actions.get(), .vlm = vlm.get(), .llm = llm.get()},
      GuardConfig::resolveAssessment());

  const GuardServiceConfig guardConfig = GuardConfig::resolveService();

  std::shared_ptr<NatsBus> natsBus;
  const std::string natsUrl = ConfigService::getString("nats.url");
  if (natsUrl.empty()) {
    LOG_INFO << "NATS not configured; guard consumer disabled";
  }
  else {
    natsBus = std::make_shared<NatsBus>();
    if (natsBus->connect())
      LOG_INFO << "NATS event bus connected to " << natsBus->options().url;
    else
      LOG_WARN << "NATS unavailable at " << natsUrl
               << "; guard keeps retrying the durable consumer";
  }

  const std::shared_ptr<const ResponseDirectory> responseDirectory =
      identity ? std::make_shared<IdentityResponseDirectory>(identity.get()) : nullptr;
  GuardService guardService(
      {.bus = natsBus.get(),
       .identity = identity.get(),
       .notifications = notifications.get(),
       .actions = actions.get(),
       .assessment = &assessment,
       .directory = responseDirectory},
      guardConfig);
  ResponseVerdictFeed verdictFeed(natsBus.get());
  verdictFeed.start();

  IdentityPresenceDirectory presenceDirectory(identity.get());
  NatsPresencePublisher presencePublisher(natsBus.get());
  PresenceService presence({.bus = natsBus.get(),
                            .directory = &presenceDirectory,
                            .publisher = &presencePublisher},
                           GuardConfig::resolvePresence());

  SettingsRegistry settings(guardSettingsCatalog());
  settings.onChange([&guardService, &presence](const std::vector<std::string>&) {
    guardService.refresh(GuardConfig::resolveService());
    presence.refresh(GuardConfig::resolvePresence());
  });
  const RpcListener rpcListener =
      startRpcListener({.registry = settings, .presence = presence});

  registerHealth();
  drogon::app().registerFilter(std::make_shared<DeviceFilter>());
  drogon::app().registerFilter(std::make_shared<ValidJsonFilter>());
  drogon::app().registerFilter(std::make_shared<JwtFilter>());
  drogon::app().registerFilter(std::make_shared<RoleFilter>());
  drogon::app().registerController(std::make_shared<GuardController>(
      GuardFeatureDependencies{.identity = identity.get()}));
  drogon::app().registerController(
      std::make_shared<PresenceController>(&presence));
  drogon::app().registerController(std::make_shared<ResponseController>(
      ResponseFeatureDependencies{.directory = responseDirectory, .clock = {}}));

  drogon::app().setExceptionHandler(ErrorHandler::handleException);

  drogon::app().setCustomErrorHandler(
      [](drogon::HttpStatusCode code, const drogon::HttpRequestPtr&) {
        return ErrorHandler::unmatchedRoute(code);
      });

  drogon::app().loadConfigJson(
      drogonConfig({.dbPath = db.dbPath, .listener = listener}));
  certificate_reload::watch(listener);

  drogon::app().registerBeginningAdvice([&db, &guardConfig]() {
    if (!guard_schema::migrate(db.schemaPath)) {
      LOG_FATAL << "Guard database migration failed — aborting startup";
      _exit(1);
    }
    if (!DbService::runScriptFile(db.schemaPath)) {
      LOG_FATAL << "Guard database schema failed to apply — aborting startup";
      _exit(1);
    }
    if (!guard_schema::seedEnvironments(
            guard_schedule::environmentSeed(guardConfig))) {
      LOG_FATAL << "Guard default environment failed to seed — aborting "
                   "startup";
      _exit(1);
    }
    DbService::applyPragmas();
  });

  LOG_INFO << "Listening on " << listener.host << ":" << listener.port
           << (listener.tls ? " (TLS" : " (plain") << ", cert "
           << listener.certPath << "); guard database " << db.dbPath;

  drogon::app().registerBeginningAdvice([&guardService, &presence]() {
    guardService.start();
    presence.start();
  });

  shutdown_signal::onQuit(
      [path = db.dbPath] { DbService::freezeClient(path); });
  shutdown_signal::onStop(
      {.name = "guard-rpc",
       .requestStop = [&rpcListener] { stopRpcListener(rpcListener); },
       .drained = [] { return true; }});
  shutdown_signal::onStop(shutdown_signal::drainOf(presence, "guard-presence"));
  shutdown_signal::onStop(shutdown_signal::drainOf(guardService, "guard"));

  std::unique_ptr<MdnsService> mdnsService;
  drogon::app().registerBeginningAdvice([&mdnsService, &listener]() {
    mdnsService = std::make_unique<MdnsService>(
        routeAnnouncements({.port = listener.port, .tls = listener.tls}));
    mdnsService->initialize();
  });

  drogon::app().setThreadNum(0).run();
  stopRpcListener(rpcListener);
  return 0;
}
