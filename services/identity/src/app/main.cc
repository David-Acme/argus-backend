#include <app/rpc/identity-rpc-service.hxx>
#include <app/rpc/identity-sync-rpc-service.hxx>
#include <app/rpc/identity-voiceprint-rpc-service.hxx>
#include <auth/auth-access.hxx>
#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/remote-config.hxx>
#include <auth/remote-gate.hxx>
#include <auth/role-filter.hxx>
#include <auth/valid-json-filter.hxx>
#include <cert/cert-service.hxx>
#include <config/config-service.hxx>
#include <config/identity-config.hxx>
#include <drogon/drogon.h>
#include <feature/face-upgrade/services/face-upgrade-service.hxx>
#include <feature/invitation/controllers/invitation-controller.hxx>
#include <feature/pairing/controllers/pairing-controller.hxx>
#include <feature/pairing/infra/pairing-banner.hxx>
#include <feature/retention/services/candidate-retention-service.hxx>
#include <feature/user/controllers/portrait-preview-controller.hxx>
#include <feature/user/controllers/user-controller.hxx>
#include <feature/user/services/nats-identity-change-sink.hxx>
#include <feature/privacy/controllers/privacy-controller.hxx>
#include <feature/voiceprint/controllers/voiceprint-controller.hxx>
#include <feature/voiceprint/repositories/voice-profile/voice-profile-repository.hxx>
#include <feature/voiceprint/services/embedding/speaker-embedding-service.hxx>
#include <feature/voiceprint/services/index/voiceprint-index.hxx>
#include <grpcpp/grpcpp.h>
#include <http/cors.hxx>
#include <http/error-handler.hxx>
#include <http/health-controller.hxx>
#include <http/listener-config.hxx>
#include <http/route-announcements.hxx>
#include <json/value.h>
#include <mdns/mdns-service.hxx>
#include <memory>
#include <nats/nats-bus.hxx>
#include <runtime/shutdown-signal.hxx>
#include <runtime/log-output.hxx>
#include <shared/repositories/face-embedding/face-embedding-repository.hxx>
#include <shared/services/face/face-service.hxx>
#include <shared/services/storage/private-portrait-service.hxx>
#include <sqlite/db-service.hxx>
#include <string>
#include <sync/identity-change-sink.hxx>
#include <sync/sync-client.hxx>
#include <sync/sync-control-sink.hxx>
#include <unistd.h>

namespace
{

constexpr int kMaxRpcReceiveBytes = 12 * 1024 * 1024;
constexpr double kVoiceCallSweepSeconds = 60.0;
constexpr double kVoiceSamplePurgeSeconds = 3600.0;

struct DrogonConfigInput
{
  const IdentityDbConfig& identityDb;
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
  client["filename"] = input.identityDb.dbPath;
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

drogon::Task<std::optional<std::string>> readStoredPortrait(int64_t userId)
{
  const PrivatePortraitService portraits;
  auto portrait = co_await portraits.read(userId);
  if (!portrait)
    co_return std::nullopt;
  co_return std::move(portrait->bytes);
}

drogon::Task<void> upgradeFaces(const FaceUpgradeService* service)
{
  co_await service->run();
}

}

int main()
{
  log_output::flushEachLine();
  DbService::enableUriFilenames();

  ConfigService::load("config.toml");

  const IdentityDbConfig identityDb = IdentityConfig::resolveDb();
  ConfigService::setRuntimeString("database.file", identityDb.dbPath);
  const ListenerConfig listener = IdentityConfig::resolveListener();
  const RemoteConfig remote = RemoteConfig::resolve();
  const IdentityRpcConfig rpc = IdentityConfig::resolveRpc();
  const IdentitySyncControlConfig syncControl =
      IdentityConfig::resolveSyncControl();
  const IdentityFaceConfig face = IdentityConfig::resolveFace();
  const IdentityVoiceprintConfig voiceprint =
      IdentityConfig::resolveVoiceprint();

  requireDistinctTunnelPort(listener, remote);

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

  drogon::app().loadConfigJson(
      drogonConfig({.identityDb = identityDb,
                    .listener = listener,
                    .remote = remote}));

  drogon::app().registerFilter(std::make_shared<DeviceFilter>());
  drogon::app().registerFilter(std::make_shared<ValidJsonFilter>());
  drogon::app().registerFilter(std::make_shared<JwtFilter>());
  drogon::app().registerFilter(std::make_shared<RoleFilter>());

  drogon::app().registerController(std::make_shared<InvitationController>());
  drogon::app().registerController(std::make_shared<PairingController>());
  drogon::app().registerController(std::make_shared<UserController>());
  drogon::app().registerController(
      std::make_shared<PortraitPreviewController>());
  drogon::app().registerController(std::make_shared<VoiceprintController>());
  drogon::app().registerController(std::make_shared<PrivacyController>());

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
                               }},
                              {"voiceprint", []() {
                                 Json::Value status(Json::objectValue);
                                 const auto& engine =
                                     SpeakerEmbeddingService::instance();
                                 status["loaded"] = engine.isLoaded();
                                 status["model"] = engine.modelId();
                                 status["indexed"] = static_cast<Json::UInt64>(
                                     VoiceprintIndex::instance().size());
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
  IdentityVoiceprintRpcService voiceprintRpcService(
      {.fleetSecret = rpc.secret, .voiceprint = voiceprint});
  grpc::ServerBuilder rpcBuilder;
  rpcBuilder.SetMaxReceiveMessageSize(kMaxRpcReceiveBytes);
  rpcBuilder.AddListeningPort(rpc.listener.host + ":" +
                                  std::to_string(rpc.listener.port),
                              grpc::InsecureServerCredentials());
  rpcBuilder.RegisterService(&rpcService);
  rpcBuilder.RegisterService(&syncRpcService);
  rpcBuilder.RegisterService(&voiceprintRpcService);
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
  if (remote.tunnelPort != 0)
    LOG_INFO << "Remote tunnel listener on port " << remote.tunnelPort
             << (remote.enabled ? " (remote requests allowed)"
                                : " (remote pairing and registration refused)");

  const FaceUpgradeService faceUpgrade(readStoredPortrait);
  drogon::app().registerBeginningAdvice([&identityDb, &identitySink, &face,
                                         &voiceprint, &faceUpgrade]() {
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

    VoiceProfileRepository::migrateLegacy();
    FaceEmbeddingRepository::ensureModelColumn();

    DbService::applyPragmas();

    if (identitySink)
      identitySink->reconcile();

    if (face.enabled) {
      FaceService::instance().init();
      if (!FaceService::instance().isLoaded())
        LOG_WARN << "FaceService not loaded — facial login disabled";
      else
        drogon::async_run(
            [service = &faceUpgrade] { return upgradeFaces(service); });
    }
    else {
      FaceService::instance().disable();
      LOG_INFO << "FaceService disabled by configuration";
    }

    auto& speaker = SpeakerEmbeddingService::instance();
    if (!voiceprint.enabled) {
      speaker.disable();
      LOG_INFO << "Voice recognition disabled by configuration";
    }
    else if (speaker.init(voiceprint.modelPath)) {
      VoiceprintIndex::instance().init(
          {.dims = speaker.dims(), .model = speaker.modelId()});
    }

    if (!CertService::init())
      LOG_WARN << "PKI not loaded — pairing disabled";
  });

  shutdown_signal::onQuit(
      [dbPath = identityDb.dbPath] { DbService::freezeClient(dbPath); });

  CandidateRetentionService candidateRetention(IdentityConfig::resolveRetention());
  std::unique_ptr<MdnsService> mdnsService;
  drogon::app().registerBeginningAdvice([&voiceprintRpcService]() {
    drogon::app().getLoop()->runEvery(
        kVoiceCallSweepSeconds,
        [&voiceprintRpcService]() { voiceprintRpcService.sweepIdleCalls(); });
    drogon::app().getLoop()->runEvery(
        kVoiceSamplePurgeSeconds, [&voiceprintRpcService]() {
          voiceprintRpcService.purgeExpiredSamples();
        });
  });
  drogon::app().registerBeginningAdvice(
      [&mdnsService, &listener, &candidateRetention]() {
        mdnsService = std::make_unique<MdnsService>(
            routeAnnouncements({.port = listener.port, .tls = listener.tls}));
        mdnsService->initialize();
        candidateRetention.start();
        pairing_banner::printWhenUnpaired(listener.port);
      });

  drogon::app().setThreadNum(0).run();

  if (rpcServer)
    rpcServer->Shutdown();
  SpeakerEmbeddingService::instance().shutdown();
  return 0;
}
