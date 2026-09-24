#include <app/rpc/identity-rpc-service.hxx>
#include <app/rpc/identity-sync-rpc-service.hxx>
#include <auth/auth-access.hxx>
#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/role-filter.hxx>
#include <auth/valid-json-filter.hxx>
#include <cert/cert-service.hxx>
#include <config/config-service.hxx>
#include <config/identity-config.hxx>
#include <drogon/drogon.h>
#include <feature/invitation/controllers/invitation-controller.hxx>
#include <feature/pairing/controllers/pairing-controller.hxx>
#include <feature/user/controllers/portrait-preview-controller.hxx>
#include <feature/user/controllers/user-controller.hxx>
#include <feature/user/services/nats-identity-change-sink.hxx>
#include <grpcpp/grpcpp.h>
#include <http/cors.hxx>
#include <http/error-handler.hxx>
#include <http/health-controller.hxx>
#include <http/listener-config.hxx>
#include <json/value.h>
#include <memory>
#include <nats/nats-bus.hxx>
#include <runtime/shutdown-signal.hxx>
#include <shared/services/face/face-service.hxx>
#include <sqlite/db-service.hxx>
#include <string>
#include <sync/identity-change-sink.hxx>
#include <sync/sync-client.hxx>
#include <sync/sync-control-sink.hxx>
#include <unistd.h>

namespace
{

Json::Value drogonConfig(const IdentityDbConfig& identityDb,
                         const ListenerConfig& listener)
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

  return config;
}

}

int main()
{
  DbService::enableUriFilenames();

  ConfigService::load("config.toml");

  const IdentityDbConfig identityDb = IdentityConfig::resolveDb();
  ConfigService::setRuntimeString("database.file", identityDb.dbPath);
  const ListenerConfig listener = IdentityConfig::resolveListener();
  const IdentityRpcConfig rpc = IdentityConfig::resolveRpc();
  const IdentitySyncControlConfig syncControl =
      IdentityConfig::resolveSyncControl();
  const IdentityFaceConfig face = IdentityConfig::resolveFace();

  std::shared_ptr<SyncClient> controlClient;
  if (syncControl.target.empty()) {
    LOG_INFO << "Sync control leg unconfigured; the imperative leg stays "
                "uninstalled";
  }
  else {
    controlClient = std::make_shared<SyncClient>(SyncClientConfig{
        .target = syncControl.target, .fleetSecret = syncControl.secret});
    sync_control::setSink(controlClient.get());
    LOG_INFO << "Sync control leg -> gRPC " << syncControl.target;
  }

  drogon::app().loadConfigJson(drogonConfig(identityDb, listener));

  drogon::app().registerFilter(std::make_shared<DeviceFilter>());
  drogon::app().registerFilter(std::make_shared<ValidJsonFilter>());
  drogon::app().registerFilter(std::make_shared<JwtFilter>());
  drogon::app().registerFilter(std::make_shared<RoleFilter>());

  drogon::app().registerController(std::make_shared<InvitationController>());
  drogon::app().registerController(std::make_shared<PairingController>());
  drogon::app().registerController(std::make_shared<UserController>());
  drogon::app().registerController(
      std::make_shared<PortraitPreviewController>());

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

  const std::string natsUrl = ConfigService::getString("nats.url");
  std::shared_ptr<NatsBus> natsBus;
  std::shared_ptr<NatsIdentityChangeSink> identitySink;
  if (natsUrl.empty()) {
    LOG_INFO << "NATS not configured; the identity change feed is disabled";
  }
  else {
    natsBus = std::make_shared<NatsBus>();
    const bool connected = natsBus->connect();
    identitySink = std::make_shared<NatsIdentityChangeSink>(
        natsBus, NatsIdentityChangeSink::Config{});
    identity_change::setSink(identitySink.get());
    shutdown_signal::onStop(
        shutdown_signal::drainOf(*identitySink, "identity-change"));
    if (connected)
      LOG_INFO << "NATS event bus connected to " << natsBus->options().url;
    else
      LOG_WARN << "NATS unavailable at " << natsUrl
               << "; subscriptions stay pending until reconnected";
  }

  const std::weak_ptr<NatsBus> healthBus = natsBus;
  drogon::app().registerController(std::make_shared<HealthController>(
      HealthStatus{.serviceName = "argus-identity",
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
                               }},
                              {"face", []() {
                                 Json::Value status(Json::objectValue);
                                 status["loaded"] =
                                     FaceService::instance().isLoaded();
                                 return status;
                               }}}}));

  if (rpc.reachableBeyondLoopback() && rpc.secret.empty()) {
    LOG_FATAL << "[server] host " << rpc.listener.host
              << " is reachable beyond loopback and validates tokens for the "
                 "whole fleet: set [identity] rpc_secret (and the same value "
                 "in every service's config) — aborting startup";
    _exit(1);
  }

  IdentityRpcService rpcService({.bus = natsBus,
                                 .fleetSecret = rpc.secret,
                                 .auth = filterAuthClient()});
  IdentitySyncRpcService syncRpcService({.fleetSecret = rpc.secret});
  grpc::ServerBuilder rpcBuilder;
  rpcBuilder.AddListeningPort(rpc.listener.host + ":" +
                                  std::to_string(rpc.listener.port),
                              grpc::InsecureServerCredentials());
  rpcBuilder.RegisterService(&rpcService);
  rpcBuilder.RegisterService(&syncRpcService);
  std::unique_ptr<grpc::Server> rpcServer(rpcBuilder.BuildAndStart());
  if (rpcServer)
    LOG_INFO << "Identity RPC listening on " << rpc.listener.host << ":"
             << rpc.listener.port << " (cleartext, "
             << (rpc.secret.empty() ? "loopback only, no fleet secret"
                                    : "fleet secret required")
             << ")";
  else
    LOG_WARN << "Identity RPC failed to listen on " << rpc.listener.host << ":"
             << rpc.listener.port
             << "; no service can reach this one over gRPC";

  LOG_INFO << "Listening on " << listener.host << ":" << listener.port
           << (listener.tls ? " (TLS" : " (plain") << ", cert "
           << listener.certPath << "); identity database " << identityDb.dbPath
           << "; gRPC on " << rpc.listener.host << ":" << rpc.listener.port;

  drogon::app().registerBeginningAdvice([&identityDb, &identitySink, &face]() {
    DbService::installExtensions();

    if (!DbService::runScriptFile(identityDb.schemaPath)) {
      LOG_FATAL << "Identity database schema failed to apply — aborting startup";
      _exit(1);
    }

    const auto personColumns = DbService::client()->execSqlSync(
        "SELECT COUNT(*) AS total FROM pragma_table_info('person') "
        "WHERE name = 'status'");
    if (personColumns.empty() || personColumns.front()["total"].as<int>() == 0)
      DbService::client()->execSqlSync(
          "ALTER TABLE person ADD COLUMN status TEXT NOT NULL DEFAULT 'known'");

    DbService::applyPragmas();

    if (identitySink)
      identitySink->reconcile();

    if (face.enabled) {
      FaceService::instance().init();
      if (!FaceService::instance().isLoaded())
        LOG_WARN << "FaceService not loaded — facial login disabled";
    }
    else {
      LOG_INFO << "FaceService disabled by configuration";
    }

    if (!CertService::init())
      LOG_WARN << "PKI not loaded — pairing disabled";
  });

  shutdown_signal::onQuit(
      [dbPath = identityDb.dbPath] { DbService::freezeClient(dbPath); });

  drogon::app().setThreadNum(0).run();

  if (rpcServer)
    rpcServer->Shutdown();
  return 0;
}
