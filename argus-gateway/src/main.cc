#include <controllers/health-controller.hxx>
#include <drogon/drogon.h>
#include <identity/identity-config.hxx>
#include <identity/identity-registrar.hxx>
#include <proxy/proxy-config.hxx>
#include <proxy/reverse-proxy.hxx>
#include <server/listener-config.hxx>
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
#include <sync/camera-fan-out.hxx>
#include <sync/sync-registrar.hxx>
#include <sync/sync-relay.hxx>
#include <unistd.h>

#include <cstdint>
#include <filesystem>
#include <stdexcept>
#include <string>

namespace
{

Json::Value drogonConfig(const IdentityDbConfig& identityDb,
                         const ListenerConfig& listener,
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

  config["listeners"] = listenerJson(listener);

  if (!proxy.upstreamUrl.empty()) {
    Json::Value plugins(Json::arrayValue);
    Json::Value proxyPlugin(Json::objectValue);
    proxyPlugin["name"] = "gateway_proxy::SimpleReverseProxy";
    proxyPlugin["dependencies"] = Json::Value(Json::arrayValue);
    Json::Value proxyConfig(Json::objectValue);
    Json::Value backends(Json::arrayValue);
    backends.append(proxy.upstreamUrl);
    proxyConfig["backends"] = backends;
    Json::Value exclusions(Json::arrayValue);
    for (const auto& prefix : proxy.exclusions)
      exclusions.append(prefix);
    proxyConfig["exclusions"] = exclusions;
    if (!proxy.cameraProxyUrl.empty()) {
      // Ruling X routing split: the two-segment /camera and /zone CRUD goes
      // to argus-camera; the deeper control paths (/camera/{id}/ptz, preset,
      // settings, status, presets, capabilities, talk) miss the segment cap
      // and fall through to the legacy backend.
      Json::Value routes(Json::arrayValue);
      Json::Value cameraRoute(Json::objectValue);
      Json::Value prefixes(Json::arrayValue);
      prefixes.append("/camera");
      prefixes.append("/zone");
      cameraRoute["prefixes"] = prefixes;
      cameraRoute["max_segments"] = 2;
      cameraRoute["backend"] = proxy.cameraProxyUrl;
      routes.append(cameraRoute);
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

void logRouting(const ProxyConfig& proxy, const ListenerConfig& listener)
{
  LOG_INFO << "Listening on " << listener.host << ":" << listener.port
           << (listener.tls ? " (TLS" : " (plain")
           << ", cert " << listener.certPath << ")";
  if (proxy.upstreamUrl.empty()) {
    LOG_INFO << "Reverse proxy disabled: the gateway serves its routes only";
    return;
  }
  LOG_INFO << "Reverse proxy -> " << proxy.upstreamUrl
           << " (excluded, gateway-native:";
  for (const auto& prefix : proxy.exclusions)
    LOG_INFO << "  " << prefix;
  LOG_INFO << ")";
}

} // namespace

int main()
{
  // SQLite URI filenames must be configured before the first sqlite3_open
  // opens the read-only legacy database below.
  DbService::enableUriFilenames();

  ConfigService::load("config.toml");

  const IdentityDbConfig identityDb = IdentityConfig::resolveDb();
  // One database drives the whole identity domain: the Drogon default client
  // and VecDb (face-db) both read the config-driven path below.
  ConfigService::setRuntimeString("database.file", identityDb.dbPath);

  drogon::app().registerController(std::make_shared<HealthController>());
  const IdentityRegistrationStats identity =
      registerIdentitySurface();
  LOG_INFO << "Identity surface registered: " << identity.controllers
           << " controllers, " << identity.filters << " filters";

  const LegacySyncConfig legacySync = LegacySyncConfig::resolve();
  const CameraSyncConfig cameraSync = CameraSyncConfig::resolve();
  // F2-2 relay split: camera:* frames relay to argus-camera, voice:* and raw
  // binary stay with the legacy (talk is TTS-load-bearing there until
  // Fase 4). A missing leg collapses to a single relay.
  std::shared_ptr<SyncForwarder> relay;
  if (!cameraSync.syncUrl.empty()) {
    relay = std::make_shared<CompositeSyncRelay>(
        std::make_shared<LegacySyncRelay>(cameraSync.syncUrl),
        std::make_shared<LegacySyncRelay>(legacySync.syncUrl));
  }
  else {
    relay = std::make_shared<LegacySyncRelay>(legacySync);
  }
  const SyncRegistrationStats sync = registerSyncSurface(relay);
  LOG_INFO << "Sync surface registered: " << sync.controllers << " controller, "
           << sync.filters << " filters"
           << (legacySync.syncUrl.empty()
                   ? " (relay disabled)"
                   : "; voice relay -> " + legacySync.syncUrl)
           << (cameraSync.syncUrl.empty()
                   ? ""
                   : "; camera relay -> " + cameraSync.syncUrl);

  const ListenerConfig listener = ListenerConfig::resolve();
  const ProxyConfig proxy = ProxyConfig::resolve();
  requireExclusionCoverage(proxy);
  logRouting(proxy, listener);

  drogon::app().loadConfigJson(drogonConfig(identityDb, listener, proxy));

  // CORS preflight is answered only for gateway-native paths; OPTIONS on
  // proxied paths is forwarded to the legacy like any other request.
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

  drogon::app().setExceptionHandler(AppConfig::handleException);

  drogon::app().setCustomErrorHandler(
      [](drogon::HttpStatusCode code, const drogon::HttpRequestPtr&) {
        if (code == drogon::k405MethodNotAllowed)
          return AppConfig::get405Response();
        return AppConfig::get404Response();
      });

  const std::string natsUrl = ConfigService::getString("nats.url");
  std::unique_ptr<NatsBus> natsBus;
  if (natsUrl.empty()) {
    LOG_INFO << "NATS not configured; event bus disabled";
  } else {
    natsBus = std::make_unique<NatsBus>();
    if (natsBus->connect()) {
      LOG_INFO << "NATS event bus connected to " << natsBus->options().url;
      camera_fan_out::subscribeChangeFanOut(*natsBus);
    }
    else
      LOG_WARN << "NATS unavailable at " << natsUrl
               << "; continuing without it";
  }

  // Prune timers for the sync socket's rooms (the gateway serves /sync
  // natively; the backend reaches the same state through its registry).
  RoomManager roomManagerLifecycle;
  roomManagerLifecycle.init();

  std::unique_ptr<MdnsService> mdnsService;

  drogon::app().registerBeginningAdvice([&identityDb = identityDb,
                                         &legacySync = legacySync,
                                         &mdnsService]() {
    DbService::installExtensions();

    if (std::filesystem::exists(legacySync.dbPath)) {
      // Sync tables read the legacy argus.db read-only; identity keeps its
      // own writable default client above.
      const auto readOnly = drogon::orm::DbClient::newSqlite3Client(
          "filename=file:" + legacySync.dbPath + "?mode=ro", 1);
      try {
        readOnly->execSqlSync("PRAGMA busy_timeout = 5000");
      }
      catch (const std::exception& e) {
        LOG_WARN << "Read-only database pragma error: " << e.what();
      }
      DbService::setReadOnlyClient(readOnly);
      LOG_INFO << "Legacy database opened read-only: " << legacySync.dbPath;
    }
    else {
      LOG_WARN << "Legacy database not found: " << legacySync.dbPath
               << "; sync reads fall back to the default client";
    }

    // Rulings Z/X: camera, camera_stream and zone reads resolve to the
    // camera database argus-camera owns. Cross-process SQLite rules apply on
    // both sides: WAL plus busy_timeout; the gateway opens it read-only and
    // never writes.
    const std::string cameraDbPath = ConfigService::getString("camera.db");
    if (!cameraDbPath.empty() && std::filesystem::exists(cameraDbPath)) {
      const auto cameraDb = drogon::orm::DbClient::newSqlite3Client(
          "filename=file:" + cameraDbPath + "?mode=ro", 1);
      try {
        cameraDb->execSqlSync("PRAGMA busy_timeout = 5000");
      }
      catch (const std::exception& e) {
        LOG_WARN << "Camera database pragma error: " << e.what();
      }
      DbService::setCameraClient(cameraDb);
      LOG_INFO << "Camera database opened read-only: " << cameraDbPath;
    }
    else if (!cameraDbPath.empty()) {
      LOG_WARN << "Camera database not found: " << cameraDbPath
               << "; camera reads fall back to the default client";
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
  return 0;
}
