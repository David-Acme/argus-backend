#include <app/rpc/sync-control-rpc-service.hxx>
#include <config/config-service.hxx>
#include <config/sync-config.hxx>
#include <drogon/drogon.h>
#include <feature/fanout/services/audit-fan-out.hxx>
#include <feature/fanout/services/notification-delivery-consumer.hxx>
#include <feature/fanout/services/sync-fan-out.hxx>
#include <feature/transport/infra/camera-sync-gateway.hxx>
#include <feature/transport/infra/notification-sync-gateway.hxx>
#include <feature/transport/infra/productivity-sync-gateway.hxx>
#include <feature/transport/infra/sync-socket-registrar.hxx>
#include <feature/transport/infra/voice-grpc-relay.hxx>
#include <grpcpp/grpcpp.h>
#include <http/cors.hxx>
#include <http/error-handler.hxx>
#include <http/health-controller.hxx>
#include <http/listener-config.hxx>
#include <json/value.h>
#include <memory>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <runtime/shutdown-signal.hxx>
#include <shared/services/room/room-manager.hxx>
#include <sqlite/db-service.hxx>
#include <string>
#include <unistd.h>

namespace
{

Json::Value drogonConfig(const SyncDbConfig& syncDb,
                         const ListenerConfig& listener)
{
  Json::Value config = ConfigService::drogonConfig();
  if (config.isNull())
    config = Json::Value(Json::objectValue);

  Json::Value clients(Json::arrayValue);
  Json::Value client(Json::objectValue);
  client["name"] = "default";
  client["rdbms"] = "sqlite3";
  client["filename"] = syncDb.dbPath;
  client["is_fast"] = false;
  client["number_of_connections"] = 1;
  client["timeout"] = -1.0;
  clients.append(client);
  config["db_clients"] = clients;

  config["listeners"] = listenerJson(listener);

  return config;
}

}

int main()
{
  DbService::enableUriFilenames();

  ConfigService::load("config.toml");

  const SyncDbConfig syncDb = SyncConfig::resolveDb();
  const ListenerConfig listener = SyncConfig::resolveListener();
  const SyncControlConfig control = SyncConfig::resolveControl();
  const SyncUpstreams upstreams = SyncConfig::resolveUpstreams();

  const auto cameraSource =
      std::make_shared<CameraSyncGateway>(upstreams.camera);
  const auto productivitySource =
      std::make_shared<ProductivitySyncGateway>(upstreams.productivity);
  const auto notificationSource =
      std::make_shared<NotificationSyncGateway>(upstreams.notification);
  const VoiceGrpcConfig voice = VoiceGrpcConfig::resolve();
  std::shared_ptr<SyncForwarder> voiceLeg;
  if (!voice.target.empty())
    voiceLeg = std::make_shared<VoiceGrpcRelay>(voice);
  const SyncRegistrationStats sync =
      registerSyncSurface({.forwarder = voiceLeg,
                           .cameraSource = cameraSource,
                           .productivitySource = productivitySource,
                           .notificationSource = notificationSource});
  LOG_INFO << "Sync surface registered: " << sync.controllers << " controller, "
           << sync.filters << " filters; voice leg -> "
           << (voice.target.empty() ? "unconfigured (503)"
                                    : "gRPC " + voice.target)
           << (upstreams.camera.empty()
                   ? "; camera tables -> unconfigured source (503)"
                   : "; camera tables -> gRPC " + upstreams.camera)
           << (upstreams.productivity.empty()
                   ? "; productivity leg -> unconfigured source (503)"
                   : "; productivity leg -> gRPC " + upstreams.productivity)
           << (upstreams.notification.empty()
                   ? "; notification leg -> unconfigured source (503)"
                   : "; notification leg -> gRPC " + upstreams.notification);

  drogon::app().loadConfigJson(drogonConfig(syncDb, listener));

  drogon::app().registerPreRoutingAdvice(
      [](const drogon::HttpRequestPtr& req, drogon::AdviceCallback&& cb,
         drogon::AdviceChainCallback&& chain) {
        if (req->method() == drogon::Options) {
          Cors::handleOptions(req, std::move(cb));
          return;
        }
        chain();
      });
  drogon::app().registerPostHandlingAdvice(
      [](const drogon::HttpRequestPtr&, const drogon::HttpResponsePtr& resp) {
        Cors::apply(resp);
      });

  drogon::app().setExceptionHandler(ErrorHandler::handleException);

  drogon::app().setCustomErrorHandler(
      [](drogon::HttpStatusCode code, const drogon::HttpRequestPtr&) {
        return ErrorHandler::unmatchedRoute(code);
      });

  std::shared_ptr<NotificationDeliveryConsumer> deliveryConsumer;
  AuditFanOut auditFanOut;
  const std::string natsUrl = ConfigService::getString("nats.url");
  std::shared_ptr<NatsBus> natsBus;
  if (natsUrl.empty()) {
    LOG_INFO << "NATS not configured; event bus disabled";
  }
  else {
    natsBus = std::make_shared<NatsBus>();
    const bool connected = natsBus->connect();
    sync_fan_out::subscribeChangeFanOut(*natsBus, auditFanOut);
    sync_fan_out::subscribeActionJournal(*natsBus, auditFanOut);
    deliveryConsumer = std::make_shared<NotificationDeliveryConsumer>(
        NotificationDeliveryConsumer::Dependencies{.bus = natsBus.get(),
                                                   .dispatch = {}},
        NotificationDeliveryConsumer::Config{
            .stream = std::string(nats_subject::kNotificationDeliveryStream),
            .durable = "argus-sync-delivery",
            .subject = std::string(nats_subject::kNotificationDelivery),
            .maxDeliver = 10,
            .poisonMaxAttempts = 3});
    deliveryConsumer->start();
    if (connected)
      LOG_INFO << "NATS event bus connected to " << natsBus->options().url;
    else
      LOG_WARN << "NATS unavailable at " << natsUrl
               << "; subscriptions stay pending until reconnected";
  }

  const std::weak_ptr<NatsBus> healthBus = natsBus;
  drogon::app().registerController(std::make_shared<HealthController>(
      HealthStatus{.serviceName = "argus-sync",
                   .extras = {{"nats", [healthBus]() {
                                 Json::Value status(Json::objectValue);
                                 const auto bus = healthBus.lock();
                                 if (!bus) {
                                   status["enabled"] = false;
                                   status["connected"] = false;
                                   return status;
                                 }
                                 status["enabled"] = true;
                                 status["connected"] = bus->isConnected();
                                 return status;
                               }}}}));

  if (control.reachableBeyondLoopback() && control.secret.empty()) {
    LOG_FATAL
        << "[server] host " << control.listener.host
        << " is reachable beyond loopback and injects frames into any "
           "user's room: set [sync] control_secret (and the same value in "
           "every service's config) — aborting startup";
    _exit(1);
  }

  SyncControlRpcService controlRpc(control.secret);
  grpc::ServerBuilder controlBuilder;
  controlBuilder.AddListeningPort(control.listener.host + ":" +
                                      std::to_string(control.listener.port),
                                  grpc::InsecureServerCredentials());
  controlBuilder.RegisterService(&controlRpc);
  std::unique_ptr<grpc::Server> controlServer(controlBuilder.BuildAndStart());
  if (controlServer)
    LOG_INFO << "Sync control RPC listening on " << control.listener.host << ":"
             << control.listener.port << " (cleartext, "
             << (control.secret.empty() ? "loopback only, no fleet secret"
                                        : "fleet secret required")
             << ")";
  else
    LOG_WARN << "Sync control RPC failed to listen on " << control.listener.host
             << ":" << control.listener.port
             << "; role changes and disconnects cannot reach this service";

  LOG_INFO << "Listening on " << listener.host << ":" << listener.port
           << (listener.tls ? " (TLS" : " (plain") << ", cert "
           << listener.certPath << "); sync database " << syncDb.dbPath;

  RoomManager roomManagerLifecycle;
  roomManagerLifecycle.init();

  drogon::app().registerBeginningAdvice([&syncDb = syncDb]() {
    if (!DbService::runScriptFile(syncDb.schemaPath)) {
      LOG_FATAL << "Sync database schema failed to apply — aborting startup";
      _exit(1);
    }

    DbService::applyPragmas();
  });

  shutdown_signal::onQuit(
      [dbPath = syncDb.dbPath] { DbService::freezeClient(dbPath); });

  drogon::app().setThreadNum(0).run();

  if (controlServer)
    controlServer->Shutdown();
  return 0;
}
