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
#include <chrono>
#include <memory>
#include <thread>
#include <shared/services/cert/cert-service.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/face/face-service.hxx>
#include <shared/services/mdns/mdns-service.hxx>
#include <shared/services/room/room-manager.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>
#include <shared/wrapper/nats/nats-push-intent-sink.hxx>
#include <shared/services/socket/nats-identity-change-sink.hxx>
#include <shared/wrapper/nats/nats-subject.hxx>
#include <sync/camera-fan-out.hxx>
#include <sync/camera-notifier.hxx>
#include <sync/camera-sync-source.hxx>
#include <sync/sync-registrar.hxx>
#include <sync/sync-relay.hxx>
#include <sync/voice-grpc-relay.hxx>
#include <sync/user-change-fan-out.hxx>
#include <sync/user-change-sink.hxx>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace
{

Json::Value drogonConfig(const IdentityDbConfig& identityDb,
                         const ListenerConfig& listener,
                         const RemoteConfig& remote,
                         const ProxyConfig& proxy)
{
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
  config["db_clients"] = clients;

  Json::Value listeners = listenerJson(listener);
  appendRemoteListener(listeners, remote, listener);
  config["listeners"] = listeners;

  if (!proxy.cameraProxyUrl.empty() || !proxy.productivityProxyUrl.empty()
      || !proxy.notificationProxyUrl.empty()) {
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
        // The whole camera domain goes to argus-camera: CRUD, zone CRUD and
        // the device-control paths (/camera/{id}/ptz, preset, settings,
        // status, presets, capabilities, talk), every segment depth.
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
        // Ruling AP routing split: the whole productivity domain goes to
        // argus-productivity, every method and subpath, so no path is served
        // by both sides.
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
      if (!proxy.notificationProxyUrl.empty()) {
        // Ruling AP routing split: the write-side notification surface goes
        // to argus-notification; /notification and /notification-token are
        // distinct prefixes (segment-boundary match), both two segments deep
        // at most.
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

// Ruling I: the proxy never forwards a gateway-native path. Every route the
// gateway registered must be covered by the exclusion set — checked here so
// an unlisted future route fails fast instead of being served by both sides.
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

void logRouting(const ProxyConfig& proxy, const ListenerConfig& listener,
                const RemoteConfig& remote)
{
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
  // SQLite URI filenames must be configured before the first sqlite3_open
  // opens the read-only domain databases below.
  DbService::enableUriFilenames();

  ConfigService::load("config.toml");

  const IdentityDbConfig identityDb = IdentityConfig::resolveDb();
  // One database drives the whole identity domain: the Drogon default client
  // and VecDb (face-db) both read the config-driven path below.
  ConfigService::setRuntimeString("database.file", identityDb.dbPath);

  drogon::app().registerController(std::make_shared<HealthController>(HealthStatus{.serviceName = "argus-gateway"}));
  const IdentityRegistrationStats identity =
      registerIdentitySurface();
  LOG_INFO << "Identity surface registered: " << identity.controllers
           << " controllers, " << identity.filters << " filters";

  const CameraSyncConfig cameraSync = CameraSyncConfig::resolve();
  const VoiceGrpcConfig voiceGrpc = VoiceGrpcConfig::resolve();
  const bool voiceCutover = !voiceGrpc.target.empty();
  std::shared_ptr<SyncForwarder> relay;
  if (!cameraSync.syncUrl.empty() || voiceCutover) {
    relay = std::make_shared<CompositeSyncRelay>(
        std::make_shared<LegacySyncRelay>(cameraSync.syncUrl),
        voiceCutover ? std::static_pointer_cast<SyncForwarder>(
                           std::make_shared<VoiceGrpcRelay>(voiceGrpc))
                     : std::make_shared<LegacySyncRelay>(std::string()));
  }
  else {
    relay = std::make_shared<LegacySyncRelay>(std::string());
  }
  const std::string cameraGrpcTarget =
      ConfigService::getString("camera.grpc_target");
  const auto cameraSource = std::make_shared<CameraSyncGateway>(cameraGrpcTarget);
  const SyncRegistrationStats sync = registerSyncSurface(relay, cameraSource);
  LOG_INFO << "Sync surface registered: " << sync.controllers << " controller, "
           << sync.filters << " filters"
           << (voiceCutover ? "; voice leg -> gRPC " + voiceGrpc.target
                            : "; voice leg -> unconfigured relay (503)")
           << (cameraGrpcTarget.empty()
                   ? "; camera leg -> unconfigured source (503)"
                   : "; camera leg -> gRPC " + cameraGrpcTarget)
           << (cameraSync.syncUrl.empty()
                   ? ""
                   : "; camera relay -> " + cameraSync.syncUrl);

  const ListenerConfig listener = ListenerConfig::resolveTls(7024);
  const RemoteConfig remote = RemoteConfig::resolve();
  const ProxyConfig proxy = ProxyConfig::resolve();
  requireDistinctTunnelPort(listener, remote);
  requireExclusionCoverage(proxy);
  logRouting(proxy, listener, remote);

  drogon::app().loadConfigJson(
      drogonConfig(identityDb, listener, remote, proxy));

  // Rulings CG/CJ: remote classification, the LAN-only bootstrap gate and
  // the refresh-token rate limiter run before routing and filters.
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

  // CORS preflight is answered only for gateway-native paths; OPTIONS on
  // routed paths is forwarded to the service backend like any other request.
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
  if (natsUrl.empty()) {
    LOG_INFO << "NATS not configured; event bus disabled";
  } else {
    natsBus = std::make_shared<NatsBus>();
    if (natsBus->connect()) {
      LOG_INFO << "NATS event bus connected to " << natsBus->options().url;
      camera_fan_out::subscribeChangeFanOut(*natsBus);
      camera_notifier::subscribeObjectDetected(*natsBus);
      // The gateway owns the identity domain's writes post-cutover (F1-5):
      // user/person rows change here, so the memory catalog replica feed
      // (Ruling BX) is published from this process.
      static const NatsIdentityChangeSink identitySink(natsBus);
      identity_change::setSink(&identitySink);
    }
    else
      LOG_WARN << "NATS unavailable at " << natsUrl
               << "; continuing without it";
  }

  std::shared_ptr<NatsPushIntentSink> pushIntentSink;
  if (push_intent::enabledFromConfig()) {
    if (natsBus) {
      pushIntentSink = std::make_shared<NatsPushIntentSink>(natsBus);
      push_intent::setSink(pushIntentSink.get());
      LOG_INFO << "Push intents enabled (" << nats_subject::kNotificationPushIntent
               << ")";
    } else {
      LOG_WARN << "[push] enabled but NATS unavailable; push intents disabled";
    }
  }

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

  // Prune timers for the sync socket's rooms (the gateway serves /sync
  // natively; the backend reaches the same state through its registry).
  RoomManager roomManagerLifecycle;
  roomManagerLifecycle.init();

  installUserChangeSink();

  std::unique_ptr<MdnsService> mdnsService;

  drogon::app().registerBeginningAdvice([&identityDb = identityDb,
                                         &mdnsService]() {
    DbService::installExtensions();

    // Rulings AQ/AR: the productivity sync-table reads and the notification
    // substrate resolve to the databases argus-productivity and
    // argus-notification own. Cross-process SQLite rules apply on both sides:
    // WAL plus busy_timeout. The gateway opens productivity.db read-only and
    // never runs DDL; notification.db it opens read-write because the gateway
    // NotificationService writes notifications there (camera-notifier
    // retarget), with the schema applied only by argus-notification's boot.
    const std::string productivityDbPath =
        ConfigService::getString("productivity.db");
    if (!productivityDbPath.empty() && !std::filesystem::exists(productivityDbPath)) {
      LOG_INFO << "Productivity database not present yet: "
               << productivityDbPath
               << "; waiting up to 30s for the argus-productivity boot apply";
      for (int ms = 0;
           ms < 30000 && !std::filesystem::exists(productivityDbPath);
           ms += 250)
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (!productivityDbPath.empty()
        && std::filesystem::exists(productivityDbPath)) {
      const auto productivityDb = drogon::orm::DbClient::newSqlite3Client(
          "filename=file:" + productivityDbPath + "?mode=ro", 1);
      try {
        productivityDb->execSqlSync("PRAGMA busy_timeout = 5000");
      }
      catch (const std::exception& e) {
        LOG_WARN << "Productivity database pragma error: " << e.what();
      }
      DbService::setProductivityClient(productivityDb);
      LOG_INFO << "Productivity database opened read-only: "
               << productivityDbPath;
    }
    else if (!productivityDbPath.empty()) {
      LOG_WARN << "Productivity database not found: " << productivityDbPath
               << "; productivity reads fall back to the default client";
    }

    const std::string notificationDbPath =
        ConfigService::getString("notifications.db");
    if (!notificationDbPath.empty()
        && !std::filesystem::exists(notificationDbPath)) {
      LOG_INFO << "Notification database not present yet: "
               << notificationDbPath
               << "; waiting up to 30s for the argus-notification boot apply";
      for (int ms = 0;
           ms < 30000 && !std::filesystem::exists(notificationDbPath);
           ms += 250)
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (!notificationDbPath.empty()
        && std::filesystem::exists(notificationDbPath)) {
      const auto notificationDb = drogon::orm::DbClient::newSqlite3Client(
          "filename=" + notificationDbPath, 1);
      try {
        notificationDb->execSqlSync("PRAGMA journal_mode = WAL");
        notificationDb->execSqlSync("PRAGMA busy_timeout = 5000");
      }
      catch (const std::exception& e) {
        LOG_WARN << "Notification database pragma error: " << e.what();
      }
      DbService::setNotificationClient(notificationDb);
      LOG_INFO << "Notification database opened read-write: "
               << notificationDbPath;
    }
    else if (!notificationDbPath.empty()) {
      LOG_WARN << "Notification database not found: " << notificationDbPath
               << "; notification reads fall back to the default client";
    }

    if (!DbService::runScriptFile(identityDb.schemaPath)) {
      LOG_FATAL << "Identity database schema failed to apply — aborting startup";
      _exit(1);
    }

    DbService::applyPragmas();

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

    // Same service name, type and TXT records the app discovers on the
    // legacy: identical [mdns] config keys, same certs directory.
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
