#include <drogon/DrClassMap.h>
#include <drogon/drogon.h>
#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <http/cors.hxx>
#include <http/error-handler.hxx>
#include <http/health-controller.hxx>
#include <http/listener-config.hxx>
#include <identity/identity-client.hxx>
#include <proxy/proxy-config.hxx>
#include <proxy/reverse-proxy.hxx>
#include <server/remote-config.hxx>
#include <server/remote-gate.hxx>
#include <json/value.h>
#include <mdns/mdns-service.hxx>
#include <memory>
#include <cert/cert-service.hxx>
#include <config/config-service.hxx>
#include <sqlite/db-service.hxx>
#include <nats/nats-bus.hxx>
#include <runtime/shutdown-signal.hxx>
#include <sync/camera-notifier.hxx>
#include <sync/camera-stream-relay.hxx>
#include <sync/camera-stream-socket.hxx>
#include <sync/sync-client.hxx>
#include <unistd.h>

#include <stdexcept>
#include <string>

namespace
{

template <typename T>
void requireFilter()
{
  if (!drogon::DrClassMap::getSingleInstance<T>())
    throw std::runtime_error(std::string(T::classTypeName())
                             + " is not registered");
}

void registerGatewayFilters()
{
  drogon::app().registerFilter(std::make_shared<DeviceFilter>());
  drogon::app().registerFilter(std::make_shared<JwtFilter>());

  requireFilter<DeviceFilter>();
  requireFilter<JwtFilter>();

  LOG_INFO << "Gateway filters registered: DeviceFilter, JwtFilter";
}

struct DrogonConfigInput
{
  const std::string& gatewayDbPath;
  const ListenerConfig& listener;
  const RemoteConfig& remote;
  const ProxyConfig& proxy;
};

Json::Value drogonConfig(const DrogonConfigInput& input)
{
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
  client["filename"] = gatewayDbPath;
  client["is_fast"] = false;
  client["number_of_connections"] = 1;
  client["timeout"] = -1.0;
  clients.append(client);
  config["db_clients"] = clients;

  Json::Value listeners = listenerJson(listener);
  appendRemoteListener(
      {.listeners = listeners, .remote = remote, .base = listener});
  config["listeners"] = listeners;

  if (!proxy.authProxyUrl.empty() || !proxy.identityProxyUrl.empty()
      || !proxy.cameraProxyUrl.empty()
      || !proxy.productivityProxyUrl.empty()
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
      if (!proxy.authProxyUrl.empty()) {
        Json::Value authRoute(Json::objectValue);
        Json::Value prefixes(Json::arrayValue);
        prefixes.append("/auth");
        authRoute["prefixes"] = prefixes;
        authRoute["max_segments"] = 4;
        authRoute["backend"] = proxy.authProxyUrl;
        authRoute["validate_cert"] = false;
        routes.append(authRoute);
      }
      if (!proxy.identityProxyUrl.empty()) {
        Json::Value identityRoute(Json::objectValue);
        Json::Value prefixes(Json::arrayValue);
        prefixes.append("/invitation");
        prefixes.append("/pairing");
        prefixes.append("/portrait-preview");
        prefixes.append("/user");
        identityRoute["prefixes"] = prefixes;
        identityRoute["max_segments"] = 4;
        identityRoute["backend"] = proxy.identityProxyUrl;
        identityRoute["validate_cert"] = false;
        routes.append(identityRoute);
      }
      if (!proxy.cameraProxyUrl.empty()) {
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
        Json::Value guardRoute(Json::objectValue);
        Json::Value prefixes(Json::arrayValue);
        prefixes.append("/guard");
        guardRoute["prefixes"] = prefixes;
        guardRoute["max_segments"] = 5;
        guardRoute["backend"] = proxy.guardProxyUrl;
        routes.append(guardRoute);
      }
      if (!proxy.notificationProxyUrl.empty()) {
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
      && proxy.notificationProxyUrl.empty() && proxy.authProxyUrl.empty()
      && proxy.identityProxyUrl.empty()) {
    LOG_INFO << "Reverse proxy disabled: the gateway serves its routes only";
    return;
  }
  LOG_INFO << "Reverse proxy excluded paths (gateway-native):";
  for (const auto& prefix : proxy.exclusions)
    LOG_INFO << "  " << prefix;
}

}

int main()
{
  DbService::enableUriFilenames();

  ConfigService::load("config.toml");

  registerGatewayFilters();

  std::string gatewayDbPath = ConfigService::getString("gateway.db");
  if (gatewayDbPath.empty())
    gatewayDbPath = "database/gateway.db";
  std::string gatewaySchemaPath = ConfigService::getString("gateway.schema");
  if (gatewaySchemaPath.empty())
    gatewaySchemaPath = "services/gateway/database/schema.sql";

  const CameraStreamConfig cameraStream = CameraStreamConfig::resolve();
  const std::string notificationGrpcTarget =
      ConfigService::getString("notifications.grpc_target");

  const std::string controlTarget =
      ConfigService::getString("sync.control_target");
  std::shared_ptr<SyncClient> controlClient;
  if (controlTarget.empty()) {
    LOG_INFO << "Sync control leg unconfigured; the imperative leg stays "
                "uninstalled";
  }
  else {
    controlClient = std::make_shared<SyncClient>(SyncClientConfig{
        .target = controlTarget,
        .fleetSecret = ConfigService::getString("sync.control_secret")});
    sync_control::setSink(controlClient.get());
    LOG_INFO << "Sync control leg -> gRPC " << controlTarget;
  }

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

  RemoteGate remoteGate(remote);
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

  drogon::app().loadConfigJson(
      drogonConfig({.gatewayDbPath = gatewayDbPath,
                    .listener = listener,
                    .remote = remote,
                    .proxy = proxy}));

  drogon::app().registerPreRoutingAdvice(
      [&proxy](const drogon::HttpRequestPtr& req,
               drogon::AdviceCallback&& cb,
               drogon::AdviceChainCallback&& chain) {
        if (req->method() == drogon::Options
            && isGatewayNativePath(req->path(), proxy.exclusions)) {
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

  const std::string natsUrl = ConfigService::getString("nats.url");
  std::shared_ptr<NatsBus> natsBus;
  CameraNotificationPolicy* fallbackPolicy = nullptr;
  if (natsUrl.empty()) {
    LOG_INFO << "NATS not configured; event bus disabled";
  } else {
    natsBus = std::make_shared<NatsBus>();
    const bool connected = natsBus->connect();
    const std::string identityTarget =
        ConfigService::getString("identity.target");
    std::shared_ptr<IdentityClient> identityClient;
    if (identityTarget.empty()) {
      LOG_WARN << "Identity target unconfigured; camera notifications keep "
                  "their fallback record but reach no recipient";
    }
    else {
      identityClient = std::make_shared<IdentityClient>(
          identityTarget, ConfigService::getString("identity.rpc_secret"));
    }
    fallbackPolicy = camera_notifier::subscribeObjectDetected(
        *natsBus,
        {.notificationClient =
             std::make_shared<NotificationClient>(NotificationClientConfig{
                 .target = notificationGrpcTarget,
                 .credential = ConfigService::getString(
                     "notifications.credential")}),
         .identityClient = identityClient});
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

  std::unique_ptr<MdnsService> mdnsService;
  drogon::app().registerBeginningAdvice([&gatewayDbPath = gatewayDbPath,
                                         &gatewaySchemaPath =
                                             gatewaySchemaPath,
                                         &mdnsService]() {
    DbService::installExtensions();

    DbService::setGatewayClient(drogon::app().getDbClient());
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

    if (!CertService::init())
      LOG_WARN << "PKI not loaded — pairing disabled";

    mdnsService = std::make_unique<MdnsService>();
    if (!mdnsService->initialize())
      LOG_WARN << "mDNS advertising failed";
  });

  shutdown_signal::onQuit(
      [dbPath = gatewayDbPath] { DbService::freezeClient(dbPath); });

  drogon::app()
      .setThreadNum(0)
      .run();

  return 0;
}
