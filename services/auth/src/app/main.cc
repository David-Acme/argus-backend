#include <app/rpc/auth-rpc-service.hxx>
#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/remote-config.hxx>
#include <auth/remote-gate.hxx>
#include <auth/role-filter.hxx>
#include <auth/valid-json-filter.hxx>
#include <config/auth-config.hxx>
#include <config/config-service.hxx>
#include <drogon/drogon.h>
#include <feature/auth/controllers/auth-controller.hxx>
#include <feature/auth/infra/auth-rate-gate.hxx>
#include <feature/device/repositories/device-credential/device-credential-repository.hxx>
#include <feature/device/repositories/device-login-challenge/device-login-challenge-repository.hxx>
#include <feature/session/repositories/refresh-token/refresh-token-repository.hxx>
#include <feature/session/infra/nats-presence-signal-sink.hxx>
#include <feature/session/services/auth-action-sink.hxx>
#include <feature/session/services/identity-change-consumer.hxx>
#include <feature/session/services/session-service.hxx>
#include <grpcpp/grpcpp.h>
#include <http/cors.hxx>
#include <http/error-handler.hxx>
#include <http/health-controller.hxx>
#include <http/certificate-reload.hxx>
#include <http/listener-config.hxx>
#include <http/route-announcements.hxx>
#include <identity/identity-client.hxx>
#include <json/value.h>
#include <mdns/mdns-service.hxx>
#include <memory>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <runtime/shutdown-signal.hxx>
#include <runtime/log-output.hxx>
#include <sqlite/db-service.hxx>
#include <string>
#include <sync/auth-change-sink.hxx>
#include <unistd.h>

namespace
{

constexpr int kActionRetryMs = 500;

struct DrogonConfigInput
{
  const AuthDbConfig& authDb;
  const ListenerConfig& listener;
  const RemoteConfig& remote;
};

Json::Value drogonConfig(const DrogonConfigInput& input)
{
  const ListenerConfig& listener = input.listener;
  const RemoteConfig& remote = input.remote;

  Json::Value config = ConfigService::drogonConfig();
  if (config.isNull())
    config = Json::Value(Json::objectValue);

  Json::Value clients(Json::arrayValue);
  Json::Value client(Json::objectValue);
  client["name"] = "default";
  client["rdbms"] = "sqlite3";
  client["filename"] = input.authDb.dbPath;
  client["is_fast"] = false;
  client["number_of_connections"] = 1;
  client["timeout"] = -1.0;
  clients.append(client);
  config["db_clients"] = clients;

  Json::Value listeners = listenerJson(listener);
  appendRemoteListener(
      {.listeners = listeners, .remote = remote, .base = listener});
  config["listeners"] = listeners;

  return config;
}

}

int main()
{
  log_output::flushEachLine();
  DbService::enableUriFilenames();

  ConfigService::load("config.toml");

  const AuthDbConfig authDb = AuthConfig::resolveDb();
  const ListenerConfig listener = AuthConfig::resolveListener();
  const RemoteConfig remote = RemoteConfig::resolve();
  const AuthRpcConfig rpc = AuthConfig::resolveRpc();
  const AuthIdentityConfig identity = AuthConfig::resolveIdentity();

  try {
    requireDistinctTunnelPort(listener, remote);
    requireTunnelListener(remote);
    DeviceFilter::requireFingerprintSecret();
  }
  catch (const std::exception& error) {
    LOG_FATAL << error.what() << " — aborting startup";
    _exit(1);
  }
  if (const auto problem = rpc.secretProblem()) {
    LOG_FATAL << *problem << " — aborting startup";
    _exit(1);
  }

  std::unique_ptr<IdentityClient> identityClient;
  if (!identity.target.empty())
    identityClient =
        std::make_unique<IdentityClient>(identity.target, identity.secret);

  SessionService sessions({.jwtService = JwtService{JwtRole::Issuer},
                           .refreshTokenRepository = RefreshTokenRepository{},
                           .identity = identityClient.get()},
                          SessionService::Config{
                              .contextCacheSeconds =
                                  AuthConfig::resolveContextCacheSeconds()});
  DeviceCredentialRepository deviceCredentials;
  const outbox::OutboxRepository changeOutbox = AuthActionSink::repository();
  RefreshTokenRepository refreshTokens;
  DeviceLoginChallengeRepository loginChallenges;

  drogon::app().loadConfigJson(
      drogonConfig({.authDb = authDb, .listener = listener, .remote = remote}));
  certificate_reload::watch(listener);

  drogon::app().registerFilter(std::make_shared<DeviceFilter>());
  drogon::app().registerFilter(std::make_shared<ValidJsonFilter>());
  drogon::app().registerFilter(std::make_shared<JwtFilter>());
  drogon::app().registerFilter(std::make_shared<RoleFilter>());
  drogon::app().registerController(std::make_shared<AuthController>(
      identityClient.get(),
      AuthFeatureService::Config{
          .refreshReuseGraceSeconds =
              AuthConfig::resolveRefreshReuseGraceSeconds(),
          .allowRemoteQrLogin = remote.allowQrLogin}));

  AuthRateGate rateGate(
      AuthConfig::resolveRateLimit(),
      [refreshTokens = JwtService{JwtRole::Issuer}](const std::string& token) {
        const auto claims = refreshTokens.verifyRefresh(token);
        const auto sid = claims.find("sid");
        return sid == claims.end() ? std::string{} : sid->second;
      });
  RemoteGate remoteGate(remote);

  drogon::app().registerPreRoutingAdvice(
      [&remoteGate, &remote](const drogon::HttpRequestPtr& req,
                             drogon::AdviceCallback&& cb,
                             drogon::AdviceChainCallback&& chain) {
        if (auto resp = remoteGate.check(req, requestIsRemote(req, remote))) {
          cb(resp);
          return;
        }
        chain();
      });

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
  std::unique_ptr<NatsPresenceSignalSink> presenceSink;
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
        natsBus,
        AuthActionSink::Config{
            .retryMs = kActionRetryMs,
            .actionSubject = nats_subject::kAuthUserAction,
            .streamName = nats_subject::kAuthChangeStream,
            .sessionSubject = nats_subject::kAuthSession,
            .sessionStreamName = nats_subject::kAuthSessionStream});
    auth_change::setSink(actionSink.get());
    presenceSink = std::make_unique<NatsPresenceSignalSink>(natsBus);
    sessions.setPresenceSink(presenceSink.get());
    shutdown_signal::onStop(
        shutdown_signal::drainOf(*actionSink, "auth-action"));
    LOG_INFO << "Auth action journal -> " << nats_subject::kAuthUserAction
             << "; session changes -> " << nats_subject::kAuthSession;

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
  if (remote.tunnelPort != 0)
    LOG_INFO << "Remote tunnel listener on port " << remote.tunnelPort
             << (remote.enabled ? " (remote requests allowed)"
                                : " (remote pairing and registration refused)")
             << (remote.allowQrLogin ? "; QR login allowed through the tunnel"
                                     : "; QR login refused through the tunnel");

  drogon::app().registerBeginningAdvice(
      [&authDb, &changeOutbox, &refreshTokens, &loginChallenges,
       &identityConsumer, &actionSink]() {
        if (!changeOutbox.migrateSchema() ||
            !refreshTokens.migrateLegacySchema() ||
            !loginChallenges.migrateLegacySchema() ||
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

  std::unique_ptr<MdnsService> mdnsService;
  drogon::app().registerBeginningAdvice([&mdnsService, &listener]() {
    mdnsService = std::make_unique<MdnsService>(
        routeAnnouncements({.port = listener.port, .tls = listener.tls}));
    mdnsService->initialize();
  });

  drogon::app().setThreadNum(0).run();
  sessions.setPresenceSink(nullptr);

  if (rpcServer)
    rpcServer->Shutdown();
  return 0;
}
