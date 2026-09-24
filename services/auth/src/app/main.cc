#include <app/rpc/auth-rpc-service.hxx>
#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/role-filter.hxx>
#include <auth/valid-json-filter.hxx>
#include <config/auth-config.hxx>
#include <config/config-service.hxx>
#include <drogon/drogon.h>
#include <feature/auth/controllers/auth-controller.hxx>
#include <feature/auth/infra/refresh-rate-gate.hxx>
#include <feature/device/repositories/device-credential/device-credential-repository.hxx>
#include <feature/session/repositories/change-outbox/change-outbox-repository.hxx>
#include <feature/session/repositories/refresh-token/refresh-token-repository.hxx>
#include <feature/session/services/auth-action-sink.hxx>
#include <feature/session/services/identity-change-consumer.hxx>
#include <feature/session/services/session-service.hxx>
#include <grpcpp/grpcpp.h>
#include <http/cors.hxx>
#include <http/error-handler.hxx>
#include <http/health-controller.hxx>
#include <http/listener-config.hxx>
#include <identity/identity-client.hxx>
#include <json/value.h>
#include <memory>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <runtime/shutdown-signal.hxx>
#include <sqlite/db-service.hxx>
#include <string>
#include <sync/auth-change-sink.hxx>
#include <sync/sync-client.hxx>
#include <sync/sync-control-sink.hxx>
#include <unistd.h>

namespace
{

constexpr int kActionRetryMs = 500;

Json::Value drogonConfig(const AuthDbConfig& authDb,
                         const ListenerConfig& listener)
{
  Json::Value config = ConfigService::drogonConfig();
  if (config.isNull())
    config = Json::Value(Json::objectValue);

  Json::Value clients(Json::arrayValue);
  Json::Value client(Json::objectValue);
  client["name"] = "default";
  client["rdbms"] = "sqlite3";
  client["filename"] = authDb.dbPath;
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

  const AuthDbConfig authDb = AuthConfig::resolveDb();
  const ListenerConfig listener = AuthConfig::resolveListener();
  const AuthRpcConfig rpc = AuthConfig::resolveRpc();
  const AuthIdentityConfig identity = AuthConfig::resolveIdentity();
  const AuthSyncControlConfig syncControl = AuthConfig::resolveSyncControl();

  std::unique_ptr<IdentityClient> identityClient;
  if (!identity.target.empty())
    identityClient =
        std::make_unique<IdentityClient>(identity.target, identity.secret);

  SessionService sessions({.jwtService = JwtService{},
                           .refreshTokenRepository = RefreshTokenRepository{},
                           .identity = identityClient.get()},
                          SessionService::Config{
                              .contextCacheSeconds =
                                  AuthConfig::resolveContextCacheSeconds()});
  DeviceCredentialRepository deviceCredentials;
  ChangeOutboxRepository changeOutbox;

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

  drogon::app().loadConfigJson(drogonConfig(authDb, listener));

  drogon::app().registerFilter(std::make_shared<DeviceFilter>());
  drogon::app().registerFilter(std::make_shared<ValidJsonFilter>());
  drogon::app().registerFilter(std::make_shared<JwtFilter>());
  drogon::app().registerFilter(std::make_shared<RoleFilter>());
  drogon::app().registerController(
      std::make_shared<AuthController>(identityClient.get()));

  RefreshRateGate rateGate(AuthConfig::resolveRateLimit());

  drogon::app().registerPreRoutingAdvice(
      [&rateGate](const drogon::HttpRequestPtr& req,
                  drogon::AdviceCallback&& cb,
                  drogon::AdviceChainCallback&& chain) {
        if (auto resp = rateGate.check(req)) {
          cb(resp);
          return;
        }
        chain();
      });

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
  drogon::app().registerPostHandlingAdvice(
      [&rateGate](const drogon::HttpRequestPtr& req,
                  const drogon::HttpResponsePtr& resp) {
        rateGate.recordOutcome(req, resp);
      });

  drogon::app().setExceptionHandler(ErrorHandler::handleException);

  drogon::app().setCustomErrorHandler(
      [](drogon::HttpStatusCode code, const drogon::HttpRequestPtr&) {
        return ErrorHandler::unmatchedRoute(code);
      });

  const std::string natsUrl = ConfigService::getString("nats.url");
  std::shared_ptr<NatsBus> natsBus;
  std::shared_ptr<IdentityChangeConsumer> identityConsumer;
  std::shared_ptr<AuthActionSink> actionSink;
  if (natsUrl.empty()) {
    LOG_INFO << "NATS not configured; the identity change feed is disabled";
  }
  else {
    natsBus = std::make_shared<NatsBus>();
    const bool connected = natsBus->connect();
    identityConsumer = std::make_shared<IdentityChangeConsumer>(
        IdentityChangeConsumer::Dependencies{.bus = natsBus.get(),
                                             .sessions = &sessions},
        IdentityChangeConsumer::Config{
            .stream = nats_subject::kIdentityChangeStream,
            .durable = "argus-auth-identity",
            .subject = nats_subject::kIdentityChange,
            .maxDeliver = 10});
    shutdown_signal::onStop(
        shutdown_signal::drainOf(*identityConsumer, "auth-identity"));

    actionSink = std::make_shared<AuthActionSink>(
        natsBus, AuthActionSink::Config{.retryMs = kActionRetryMs,
                                        .actionSubject =
                                            nats_subject::kAuthUserAction,
                                        .streamName =
                                            nats_subject::kAuthChangeStream});
    auth_change::setSink(actionSink.get());
    shutdown_signal::onStop(
        shutdown_signal::drainOf(*actionSink, "auth-action"));
    LOG_INFO << "Auth action journal -> " << nats_subject::kAuthUserAction;

    if (connected)
      LOG_INFO << "NATS event bus connected to " << natsBus->options().url;
    else
      LOG_WARN << "NATS unavailable at " << natsUrl
               << "; the change feed stays pending until reconnected";
  }

  const std::weak_ptr<NatsBus> healthBus = natsBus;
  drogon::app().registerController(std::make_shared<HealthController>(
      HealthStatus{.serviceName = "argus-auth",
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

  if (rpc.reachableBeyondLoopback() && rpc.secret.empty()) {
    LOG_FATAL
        << "[server] host " << rpc.listener.host
        << " is reachable beyond loopback and answers session verdicts: set "
           "[auth] rpc_secret (and the same value in every service's config) "
           "— aborting startup";
    _exit(1);
  }

  AuthRpcService rpcService({.sessions = &sessions,
                             .deviceCredentials = &deviceCredentials},
                            rpc.secret);
  grpc::ServerBuilder rpcBuilder;
  rpcBuilder.AddListeningPort(rpc.listener.host + ":" +
                                  std::to_string(rpc.listener.port),
                              grpc::InsecureServerCredentials());
  rpcBuilder.RegisterService(&rpcService);
  std::unique_ptr<grpc::Server> rpcServer(rpcBuilder.BuildAndStart());
  if (rpcServer)
    LOG_INFO << "Auth RPC listening on " << rpc.listener.host << ":"
             << rpc.listener.port << " (cleartext, "
             << (rpc.secret.empty() ? "loopback only, no fleet secret"
                                    : "fleet secret required")
             << ")";
  else
    LOG_WARN << "Auth RPC failed to listen on " << rpc.listener.host << ":"
             << rpc.listener.port
             << "; no service can validate a session against this one";

  LOG_INFO << "Listening on " << listener.host << ":" << listener.port
           << (listener.tls ? " (TLS" : " (plain") << ", cert "
           << listener.certPath << "); auth database " << authDb.dbPath
           << "; session verdicts -> "
           << (identity.target.empty() ? "unconfigured identity (401)"
                                       : "gRPC " + identity.target);

  drogon::app().registerBeginningAdvice(
      [&authDb, &changeOutbox, &identityConsumer, &actionSink]() {
        if (!changeOutbox.migrateLegacySchema() ||
            !DbService::runScriptFile(authDb.schemaPath)) {
          LOG_FATAL << "Auth database schema failed to apply — aborting startup";
          _exit(1);
        }

        DbService::applyPragmas();

        if (actionSink)
          actionSink->reconcile();

        if (identityConsumer)
          identityConsumer->start();
      });

  shutdown_signal::onQuit(
      [dbPath = authDb.dbPath] { DbService::freezeClient(dbPath); });

  drogon::app().setThreadNum(0).run();

  if (rpcServer)
    rpcServer->Shutdown();
  return 0;
}
