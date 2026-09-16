#include <controllers/health-controller.hxx>
#include <drogon/drogon.h>
#include <identity/identity-config.hxx>
#include <identity/identity-registrar.hxx>
#include <feature/rpc/identity-rpc.hxx>
#include <proxy/proxy-config.hxx>
#include <proxy/reverse-proxy.hxx>
#include <server/listener-config.hxx>
#include <server/remote-config.hxx>
#include <server/remote-gate.hxx>
#include <server/refresh-rate-limiter.hxx>
#include <config/app-config.hxx>
#include <json/value.h>
#include <memory>
#include <shared/services/cert/cert-service.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/face/face-service.hxx>
#include <shared/services/mdns/mdns-service.hxx>
#include <shared/services/room/room-manager.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>
#include <shared/wrapper/nats/nats-subject.hxx>
#include <shared/services/socket/nats-identity-change-sink.hxx>
#include <sync/camera-fan-out.hxx>
#include <sync/camera-notifier.hxx>
#include <sync/camera-stream-relay.hxx>
#include <sync/camera-stream-socket.hxx>
#include <sync/camera-sync-source.hxx>
#include <sync/notification-sync-source.hxx>
#include <sync/notification-delivery-consumer.hxx>
#include <sync/productivity-sync-source.hxx>
#include <sync/sync-registrar.hxx>
#include <sync/voice-grpc-relay.hxx>
#include <sync/user-change-fan-out.hxx>
#include <unistd.h>

#include <stdexcept>
#include <string>

namespace
{

struct DrogonConfigInput
{
  const IdentityDbConfig& identityDb;
  const std::string& gatewayDbPath;
  const ListenerConfig& listener;
  const RemoteConfig& remote;
  const ProxyConfig& proxy;
};

Json::Value drogonConfig(const DrogonConfigInput& input)
{
  const IdentityDbConfig& identityDb = input.identityDb;
  const std::string& gatewayDbPath = input.gatewayDbPath;
  const ListenerConfig& listener = input.listener;
  const RemoteConfig& remote = input.remote;
  const ProxyConfig& proxy = input.proxy;

  Json::Value config = ConfigService::drogonConfig();
  if (config.isNull())
    config = Json::Value(Json::objectValue);

  Json::Value clients(Json::arrayValue);
  Json::Value client(Json::objectValue);
  client["name"] = "default";
  client["rdbms"] = "sqlite3";
  client["filename"] = identityDb.dbPath;
  client["is_fast"] = false;
  client["number_of_connections"] = 1;
  client["timeout"] = -1.0;
  clients.append(client);
  Json::Value gatewayClient(Json::objectValue);
  gatewayClient["name"] = "gateway";
  gatewayClient["rdbms"] = "sqlite3";
  gatewayClient["filename"] = gatewayDbPath;
  gatewayClient["is_fast"] = false;
  gatewayClient["number_of_connections"] = 1;
  gatewayClient["timeout"] = -1.0;
  clients.append(gatewayClient);
  config["db_clients"] = clients;

  Json::Value listeners = listenerJson(listener);
  appendRemoteListener(
      {.listeners = listeners, .remote = remote, .base = listener});
  config["listeners"] = listeners;

  if (!proxy.cameraProxyUrl.empty() || !proxy.productivityProxyUrl.empty()
      || !proxy.notificationProxyUrl.empty() ||
      !proxy.guardProxyUrl.empty()) {
    Json::Value plugins(Json::arrayValue);
    Json::Value proxyPlugin(Json::objectValue);
    proxyPlugin["name"] = "gateway_proxy::SimpleReverseProxy";
    proxyPlugin["dependencies"] = Json::Value(Json::arrayValue);
    Json::Value proxyConfig(Json::objectValue);
    Json::Value exclusions(Json::arrayValue);
    for (const auto& prefix : proxy.exclusions)
      exclusions.append(prefix);
    proxyConfig["exclusions"] = exclusions;
    {
      Json::Value routes(Json::arrayValue);
      if (!proxy.cameraProxyUrl.empty()) {
        // The whole camera domain goes to argus-camera, every segment depth.
        Json::Value cameraRoute(Json::objectValue);
        Json::Value prefixes(Json::arrayValue);
        prefixes.append("/camera");
        prefixes.append("/zone");
        cameraRoute["prefixes"] = prefixes;
        cameraRoute["max_segments"] = 8;
        cameraRoute["backend"] = proxy.cameraProxyUrl;
        routes.append(cameraRoute);
      }
      if (!proxy.productivityProxyUrl.empty()) {
        // The whole productivity domain goes to argus-productivity.
        Json::Value productivityRoute(Json::objectValue);
        Json::Value prefixes(Json::arrayValue);
        prefixes.append("/calendar-event");
        prefixes.append("/calendar-event-share");
        prefixes.append("/project");
        prefixes.append("/project-member");
        prefixes.append("/project-task");
        productivityRoute["prefixes"] = prefixes;
        productivityRoute["max_segments"] = 8;
        productivityRoute["backend"] = proxy.productivityProxyUrl;
        routes.append(productivityRoute);
      }
      if (!proxy.guardProxyUrl.empty()) {
        // The owner-only guard API: mode, incidents, decisions and guests.
        Json::Value guardRoute(Json::objectValue);
        Json::Value prefixes(Json::arrayValue);
        prefixes.append("/guard");
        guardRoute["prefixes"] = prefixes;
        guardRoute["max_segments"] = 5;
        guardRoute["backend"] = proxy.guardProxyUrl;
        routes.append(guardRoute);
      }
      if (!proxy.notificationProxyUrl.empty()) {
        // The write-side notification surface goes to argus-notification.
        Json::Value notificationRoute(Json::objectValue);
        Json::Value prefixes(Json::arrayValue);
        prefixes.append("/notification");
        prefixes.append("/notification-token");
        notificationRoute["prefixes"] = prefixes;
        notificationRoute["max_segments"] = 2;
        notificationRoute["backend"] = proxy.notificationProxyUrl;
        routes.append(notificationRoute);
      }
      proxyConfig["routes"] = routes;
    }
    proxyConfig["pipelining"] = 16;
    proxyConfig["connection_factor"] = 1;
    proxyPlugin["config"] = proxyConfig;
    plugins.append(proxyPlugin);
    config["plugins"] = plugins;
  }

  return config;
}

// Every gateway-native route must be covered by the exclusion set, fail fast.
void requireExclusionCoverage(const ProxyConfig& proxy)
{
  for (const auto& handlerInfo : drogon::app().getHandlersInfo()) {
    const auto& pattern = std::get<0>(handlerInfo);
    if (pattern.empty() || pattern.front() != '/')
      continue;
    if (!isGatewayNativePath(pattern, proxy.exclusions)) {
      throw std::runtime_error("Route " + pattern
                               + " is not in the proxy exclusion set");
    }
  }
}

struct LogRoutingInput
{
  const ProxyConfig& proxy;
  const ListenerConfig& listener;
  const RemoteConfig& remote;
};

void logRouting(const LogRoutingInput& input)
{
  const ProxyConfig& proxy = input.proxy;
  const ListenerConfig& listener = input.listener;
  const RemoteConfig& remote = input.remote;

  LOG_INFO << "Listening on " << listener.host << ":" << listener.port
           << (listener.tls ? " (TLS" : " (plain")
           << ", cert " << listener.certPath << ")";
  if (remote.tunnelPort != 0)
    LOG_INFO << "Remote tunnel listener on " << listener.host << ":"
             << remote.tunnelPort << " (pairing/register "
             << (remote.enabled ? "allowed" : "LAN-only") << ")";
  if (proxy.cameraProxyUrl.empty() && proxy.productivityProxyUrl.empty()
      && proxy.notificationProxyUrl.empty()) {
    LOG_INFO << "Reverse proxy disabled: the gateway serves its routes only";
    return;
  }
  LOG_INFO << "Reverse proxy excluded paths (gateway-native):";
  for (const auto& prefix : proxy.exclusions)
    LOG_INFO << "  " << prefix;
}

} // namespace

int main()
{
  DbService::enableUriFilenames();

  ConfigService::load("config.toml");

  const IdentityDbConfig identityDb = IdentityConfig::resolveDb();
  // One database drives the whole identity domain.
  ConfigService::setRuntimeString("database.file", identityDb.dbPath);

  std::string gatewayDbPath = ConfigService::getString("gateway.db");
  if (gatewayDbPath.empty())
    gatewayDbPath = "database/gateway.db";
  std::string gatewaySchemaPath = ConfigService::getString("gateway.schema");
  if (gatewaySchemaPath.empty())
    gatewaySchemaPath = "services/argus-gateway/database/schema.sql";

  const IdentityRegistrationStats identity =
      registerIdentitySurface();
  LOG_INFO << "Identity surface registered: " << identity.controllers
           << " controllers, " << identity.filters << " filters";

  const CameraStreamConfig cameraStream = CameraStreamConfig::resolve();
  const VoiceGrpcConfig voiceGrpc = VoiceGrpcConfig::resolve();
  const bool voiceCutover = !voiceGrpc.target.empty();
  std::shared_ptr<SyncForwarder> relay;
  if (voiceCutover)
    relay = std::make_shared<VoiceGrpcRelay>(voiceGrpc);
  const std::string cameraGrpcTarget =
      ConfigService::getString("camera.grpc_target");
  const std::string productivityGrpcTarget =
      ConfigService::getString("productivity.grpc_target");
  const std::string notificationGrpcTarget =
      ConfigService::getString("notifications.grpc_target");
  const auto cameraSource = std::make_shared<CameraSyncGateway>(cameraGrpcTarget);
  const auto productivitySource =
      std::make_shared<ProductivitySyncGateway>(productivityGrpcTarget);
  const auto notificationSource =
      std::make_shared<NotificationSyncGateway>(notificationGrpcTarget);
  const SyncRegistrationStats sync =
      registerSyncSurface({.forwarder = relay,
                           .cameraSource = cameraSource,
                           .productivitySource = productivitySource,
                           .notificationSource = notificationSource});
  LOG_INFO << "Sync surface registered: " << sync.controllers << " controller, "
           << sync.filters << " filters"
           << (voiceCutover ? "; voice leg -> gRPC " + voiceGrpc.target
                            : "; voice leg -> unconfigured (503)")
           << (cameraGrpcTarget.empty()
                   ? "; camera tables -> unconfigured source (503)"
                   : "; camera tables -> gRPC " + cameraGrpcTarget)
           << (productivityGrpcTarget.empty()
                   ? "; productivity leg -> unconfigured source (503)"
                   : "; productivity leg -> gRPC " + productivityGrpcTarget)
           << (notificationGrpcTarget.empty()
                   ? "; notification leg -> unconfigured source (503)"
                   : "; notification leg -> gRPC " + notificationGrpcTarget);

  if (!cameraStream.streamUrl.empty()) {
    const auto socket = std::make_shared<CameraStreamSocket>();
    socket->setRelay(std::make_shared<CameraStreamRelay>(cameraStream.streamUrl));
    drogon::app().registerController(socket);
    LOG_INFO << "Camera stream socket registered: /camera-stream -> "
             << cameraStream.streamUrl;
  }
  else {
    LOG_INFO << "Camera stream socket disabled";
  }

  const ListenerConfig listener = ListenerConfig::resolveTls(7024);
  const RemoteConfig remote = RemoteConfig::resolve();
  const ProxyConfig proxy = ProxyConfig::resolve();
  requireDistinctTunnelPort(listener, remote);
  requireExclusionCoverage(proxy);
  logRouting({.proxy = proxy, .listener = listener, .remote = remote});

  drogon::app().loadConfigJson(
      drogonConfig({.identityDb = identityDb,
                    .gatewayDbPath = gatewayDbPath,
                    .listener = listener,
                    .remote = remote,
                    .proxy = proxy}));

  // Remote classification and the rate limiter run before filters.
  RemoteGate remoteGate(remote,
                        std::make_shared<RefreshRateLimiter>(
                            RateLimitConfig::resolve()));
  drogon::app().registerPreRoutingAdvice(
      [&remoteGate, &remote](const drogon::HttpRequestPtr& req,
                             drogon::AdviceCallback&& cb,
                             drogon::AdviceChainCallback&& chain) {
        if (auto resp = remoteGate.check(req, requestIsRemote(req, remote))) {
          cb(std::move(resp));
          return;
        }
        chain();
      });

  // OPTIONS on routed paths is forwarded to the backend like any request.
  drogon::app().registerPreRoutingAdvice(
      [&proxy](const drogon::HttpRequestPtr& req,
               drogon::AdviceCallback&& cb,
               drogon::AdviceChainCallback&& chain) {
        if (req->method() == drogon::Options
            && isGatewayNativePath(req->path(), proxy.exclusions)) {
          AppConfig::handleOptions(req, std::move(cb));
          return;
        }
        chain();
      });

  drogon::app().registerPostHandlingAdvice(
      [](const drogon::HttpRequestPtr&, const drogon::HttpResponsePtr& resp) {
        AppConfig::applyCors(resp);
      });

  drogon::app().registerPostHandlingAdvice(
      [&remoteGate](const drogon::HttpRequestPtr& req,
                    const drogon::HttpResponsePtr& resp) {
        remoteGate.recordOutcome(req, resp);
      });

  drogon::app().setExceptionHandler(AppConfig::handleException);

  drogon::app().setCustomErrorHandler(
      [](drogon::HttpStatusCode code, const drogon::HttpRequestPtr&) {
        if (code == drogon::k405MethodNotAllowed)
          return AppConfig::get405Response();
        return AppConfig::get404Response();
      });

  const std::string natsUrl = ConfigService::getString("nats.url");
  std::shared_ptr<NatsBus> natsBus;
  std::shared_ptr<NotificationDeliveryConsumer> deliveryConsumer;
  CameraNotificationPolicy* fallbackPolicy = nullptr;
  if (natsUrl.empty()) {
    LOG_INFO << "NATS not configured; event bus disabled";
  } else {
    // Handlers register before the first successful connection; the bus
    // supervises reconnects and re-attaches every subscription.
    natsBus = std::make_shared<NatsBus>();
    const bool connected = natsBus->connect();
    camera_fan_out::subscribeChangeFanOut(*natsBus);
    fallbackPolicy = camera_notifier::subscribeObjectDetected(
        *natsBus, std::make_shared<NotificationClient>(NotificationClientConfig{
                       .target = notificationGrpcTarget,
                       .credential = ConfigService::getString(
                           "notifications.credential")}));
    // User rows change here, so the catalog replica feed publishes from here.
    static const NatsIdentityChangeSink identitySink(natsBus);
    identity_change::setSink(&identitySink);
    deliveryConsumer = std::make_shared<NotificationDeliveryConsumer>(
        NotificationDeliveryConsumer::Dependencies{.bus = natsBus.get(),
                                                   .dispatch = {}},
        NotificationDeliveryConsumer::Config{
            .stream = std::string(nats_subject::kNotificationDeliveryStream),
            .durable = "argus-gateway-delivery",
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
      HealthStatus{.serviceName = "argus-gateway",
                   .extras = {{"nats",
                               [healthBus]() {
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
                               }},
                              {"notifications_fallback",
                               [fallbackPolicy]() {
                                 Json::Value status(Json::objectValue);
                                 if (fallbackPolicy == nullptr) {
                                   status["subscribed"] = false;
                                   return status;
                                 }
                                 status["subscribed"] = true;
                                 const auto counts =
                                     fallbackPolicy->fallbackCounts();
                                 status["passed"] = Json::Int64(counts.passed);
                                 status["dropped_known"] =
                                     Json::Int64(counts.droppedKnown);
                                 status["dropped_weak_score"] =
                                     Json::Int64(counts.droppedWeakScore);
                                 status["dropped_short_dwell"] =
                                     Json::Int64(counts.droppedShortDwell);
                                 return status;
                               }}}}));

  const IdentityRpcConfig identityRpcConfig = IdentityRpcConfig::resolve();
  if (identityRpcConfig.reachableBeyondLoopback() &&
      identityRpcConfig.secret.empty()) {
    LOG_FATAL << "[identity] rpc_host " << identityRpcConfig.host
              << " is reachable beyond loopback and validates tokens for the "
                 "whole fleet: set [identity] rpc_secret (and the same value "
                 "in every service's config) — aborting startup";
    _exit(1);
  }

  IdentityRpcService identityRpc(natsBus, identityRpcConfig.secret);
  grpc::ServerBuilder identityBuilder;
  identityBuilder.AddListeningPort(
      identityRpcConfig.host + ":" + std::to_string(identityRpcConfig.port),
      grpc::InsecureServerCredentials());
  identityBuilder.RegisterService(&identityRpc);
  std::unique_ptr<grpc::Server> identityServer(identityBuilder.BuildAndStart());
  if (identityServer)
    LOG_INFO << "Identity RPC listening on " << identityRpcConfig.host << ":"
             << identityRpcConfig.port << " (cleartext, "
             << (identityRpcConfig.secret.empty()
                     ? "loopback only, no fleet secret"
                     : "fleet secret required")
             << ")";
  else
    LOG_WARN << "Identity RPC failed to listen on " << identityRpcConfig.host
             << ":" << identityRpcConfig.port;

  // Prune timers for the sync socket's rooms.
  RoomManager roomManagerLifecycle;
  roomManagerLifecycle.init();

  std::unique_ptr<MdnsService> mdnsService;
  drogon::app().registerBeginningAdvice([&identityDb = identityDb,
                                         &gatewayDbPath = gatewayDbPath,
                                         &gatewaySchemaPath =
                                             gatewaySchemaPath,
                                         &mdnsService]() {
    DbService::installExtensions();

    if (!DbService::runScriptFile(identityDb.schemaPath)) {
      LOG_FATAL << "Identity database schema failed to apply — aborting startup";
      _exit(1);
    }

    const auto personColumns = DbService::client()->execSqlSync(
        "SELECT COUNT(*) AS total FROM pragma_table_info('person') "
        "WHERE name = 'status'");
    if (personColumns.empty() ||
        personColumns.front()["total"].as<int>() == 0)
      DbService::client()->execSqlSync(
          "ALTER TABLE person ADD COLUMN status TEXT NOT NULL DEFAULT 'known'");

    DbService::applyPragmas();

    DbService::setGatewayClient(drogon::app().getDbClient("gateway"));
    if (!DbService::runScriptFile(gatewaySchemaPath,
                                  DbService::gatewayClient())) {
      LOG_ERROR << "Gateway fallback record unavailable: could not apply "
                << gatewaySchemaPath << " to " << gatewayDbPath
                << " (create the data/gateway host directory and restart); "
                   "the gateway keeps serving, fallback drops will only be "
                   "counted, not recorded";
    }
    else {
      DbService::applyPragmas(DbService::gatewayClient());
    }

    if (ConfigService::getBool("face.enabled")) {
      FaceService::instance().init();
      if (!FaceService::instance().isLoaded())
        LOG_WARN << "FaceService not loaded — facial login disabled";
    }
    else {
      LOG_INFO << "FaceService disabled by configuration";
    }

    if (!CertService::init())
      LOG_WARN << "PKI not loaded — pairing disabled";

    mdnsService = std::make_unique<MdnsService>();
    if (!mdnsService->initialize())
      LOG_WARN << "mDNS advertising failed";
  });

  drogon::app()
      .setThreadNum(0)
      .run();

  if (identityServer)
    identityServer->Shutdown();
  return 0;
}
