#include <app/rpc/identity-callers.hxx>
#include <app/rpc/identity-rpc-service.hxx>
#include <app/rpc/identity-sync-rpc-service.hxx>
#include <app/rpc/identity-voiceprint-rpc-service.hxx>
#include <feature/module-data/services/identity-module-data.hxx>
#include <feature/module-impact/services/identity-module-impact.hxx>
#include <feature/module-impact/services/identity-role-reassign.hxx>
#include <settings/settings-rpc.hxx>
#include <auth/auth-access.hxx>
#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/remote-config.hxx>
#include <auth/remote-gate.hxx>
#include <auth/module-feed.hxx>
#include <auth/role-filter.hxx>
#include <auth/valid-json-filter.hxx>
#include <cert/cert-service.hxx>
#include <config/config-service.hxx>
#include <grpc/fleet-caller-gate.hxx>
#include <grpc/grpc-server-drain.hxx>
#include <config/identity-config.hxx>
#include <drogon/drogon.h>
#include <feature/face-upgrade/services/face-upgrade-service.hxx>
#include <feature/invitation/controllers/invitation-controller.hxx>
#include <feature/invitation/services/invitation-module-revocation.hxx>
#include <feature/pairing/controllers/pairing-controller.hxx>
#include <feature/pairing/infra/pairing-banner.hxx>
#include <feature/retention/services/candidate-retention-service.hxx>
#include <feature/role-storage/services/role-check-migration.hxx>
#include <feature/user/controllers/portrait-preview-controller.hxx>
#include <feature/user/controllers/user-controller.hxx>
#include <feature/user/services/nats-identity-change-sink.hxx>
#include <feature/privacy/controllers/privacy-controller.hxx>
#include <feature/rate-gate/infra/identity-rate-gate.hxx>
#include <feature/visitor/controllers/visitor-controller.hxx>
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
#include <shared/repositories/person/person-repository.hxx>
#include <shared/repositories/user-invitation/user-invitation-repository.hxx>
#include <shared/services/face/face-service.hxx>
#include <shared/services/object-deletion/object-deletion-worker.hxx>
#include <shared/services/schema/secure-delete.hxx>
#include <shared/services/storage/private-portrait-service.hxx>
#include <sqlite/db-service.hxx>
#include <string>
#include <sync/identity-change-sink.hxx>
#include <sync/sync-client.hxx>
#include <sync/sync-control-sink.hxx>
#include <chrono>
#include <exception>
#include <utility>
#include <vector>
#include <unistd.h>

namespace
{

constexpr int kMaxRpcReceiveBytes = 12 * 1024 * 1024;
constexpr std::chrono::milliseconds kRpcDrainDeadline{2000};
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
  IdentityRpcConfig rpc = IdentityConfig::resolveRpc();
  auto settingsCredentials = settingsCallers(rpc.callers);
  withoutSettingsCaller(rpc.callers);
  const IdentitySyncControlConfig syncControl =
      IdentityConfig::resolveSyncControl();
  const IdentityFaceConfig face = IdentityConfig::resolveFace();
  const IdentityVoiceprintConfig voiceprint =
      IdentityConfig::resolveVoiceprint();

  try {
    requireDistinctTunnelPort(listener, remote);
    requireTunnelListener(remote);
    DeviceFilter::requireFingerprintSecret();
  }
  catch (const std::exception& error) {
    LOG_FATAL << error.what() << " — aborting startup";
    _exit(1);
  }

  std::shared_ptr<SyncClient> controlClient;
  if (syncControl.target.empty()) {
    LOG_INFO << "Sync control leg unconfigured; the imperative leg stays "
                "uninstalled";
  }
  else {
    controlClient = std::make_shared<SyncClient>(
        SyncClientConfig{.target = syncControl.target,
                         .credential = syncControl.credential,
                         .fleetSecret = syncControl.secret});
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
  drogon::app().registerController(std::make_shared<VisitorController>());

  RemoteGate remoteGate(remote);
  IdentityRateGate rateGate(IdentityConfig::resolveRateLimit());

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
      [&rateGate](const drogon::HttpRequestPtr& req, drogon::AdviceCallback&& cb,
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
      [&rateGate](const drogon::HttpRequestPtr& req,
                  const drogon::HttpResponsePtr& resp) {
        rateGate.recordOutcome(req, resp);
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

  const auto modules = module_gate::install({.service = "identity", .bus = natsBus});

  const InvitationModuleRevocation invitationRevocation;
  moduleGate().onChange([&invitationRevocation](const ModuleChange& change) {
    if (change.enabled)
      return;
    drogon::app().getLoop()->queueInLoop([&invitationRevocation, id = change.id] {
      drogon::async_run([&invitationRevocation, id]() -> drogon::Task<void> {
        try {
          co_await invitationRevocation.revokeModule(id);
        }
        catch (const std::exception& error) {
          LOG_WARN << "Identity: the pending invitations of " << id << " could not be revoked: " << error.what();
        }
        co_return;
      });
    });
  });

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
                                 status["liveness"] =
                                     FaceService::instance().livenessLoaded();
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

  const auto rpcGate = std::make_shared<const argus::client::FleetCallerGate>(
      argus::client::FleetGateConfig{
          .expectedCallers = identity_callers::expected(),
          .callerPairs = rpc.callers,
          .legacySecret = rpc.secret,
          .onFirstLegacy = [](const std::vector<std::string>& unpaired) {
            LOG_WARN << "Identity RPC: a caller presented the fleet-wide "
                        "[identity] rpc_secret; it reaches only what an "
                        "unpaired caller may call until every caller has its "
                        "own [rpc.callers] credential (unpaired: "
                     << unpaired.size() << ", run scripts/setup.sh or "
                        "scripts/provision-host.sh to pair them)";
          }});
  if (rpc.reachableBeyondLoopback() && rpcGate->open()) {
    LOG_FATAL << "[server] host " << rpc.listener.host
              << " is reachable beyond loopback and validates tokens for the "
                 "whole fleet: pair every caller in [rpc.callers] — aborting "
                 "startup";
    _exit(1);
  }

  IdentityRpcService rpcService({.bus = natsBus,
                                 .gate = rpcGate,
                                 .auth = filterAuthClient()});
  IdentitySyncRpcService syncRpcService({.gate = rpcGate});
  IdentityVoiceprintRpcService voiceprintRpcService(
      {.gate = rpcGate, .voiceprint = voiceprint});
  grpc::ServerBuilder rpcBuilder;
  rpcBuilder.SetMaxReceiveMessageSize(kMaxRpcReceiveBytes);
  rpcBuilder.AddListeningPort(rpc.listener.host + ":" +
                                  std::to_string(rpc.listener.port),
                              grpc::InsecureServerCredentials());
  rpcBuilder.RegisterService(&rpcService);
  rpcBuilder.RegisterService(&syncRpcService);
  rpcBuilder.RegisterService(&voiceprintRpcService);
  IdentityModuleData moduleData;
  const IdentityModuleImpact moduleImpact;
  IdentityRoleReassign roleReassign;
  SettingsRegistry noSettings({});
  std::unique_ptr<SettingsRpcService> settingsRpc;
  if (settingsCredentials.empty()) {
    LOG_INFO << "Module data RPC not served: [rpc.callers] settings is empty";
  }
  else {
    settingsRpc = std::make_unique<SettingsRpcService>(SettingsRpcInput{
        .service = "identity", .registry = &noSettings, .credentials = std::move(settingsCredentials)});
    settingsRpc->attachModuleData(moduleData);
    settingsRpc->attachModuleImpact(moduleImpact);
    settingsRpc->attachRoleReassign(roleReassign);
    rpcBuilder.RegisterService(settingsRpc.get());
  }
  std::unique_ptr<grpc::Server> rpcServer(rpcBuilder.BuildAndStart());
  const bool rpcListening = rpcServer != nullptr;
  argus::client::GrpcServerDrain rpcDrain(std::move(rpcServer),
                                          kRpcDrainDeadline);
  shutdown_signal::onStop(shutdown_signal::drainOf(rpcDrain, "identity-rpc"));
  if (rpcListening)
    LOG_INFO << "Identity RPC listening on " << rpc.listener.host << ":"
             << rpc.listener.port << " (cleartext, "
             << (rpcGate->open() ? "loopback only, no caller credential"
                                 : std::to_string(rpcGate->pairedCount()) +
                                       " paired callers" +
                                       (rpcGate->acceptsLegacy()
                                            ? ", legacy fleet secret for the "
                                              "unpaired ones"
                                            : ""))
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
                                         &voiceprint, &faceUpgrade,
                                         &invitationRevocation]() {
    DbService::installExtensions();

    if (!DbService::runScriptFile(identityDb.schemaPath)) {
      LOG_FATAL << "Identity database schema failed to apply — aborting startup";
      _exit(1);
    }

    if (!NatsIdentityChangeSink::repository().migrateSchema()) {
      LOG_FATAL << "Identity change outbox migration failed — aborting startup";
      _exit(1);
    }

    static_cast<void>(role_check_migration::applyToFile(identityDb.dbPath));

    PersonRepository::ensureColumns();
    UserInvitationRepository::ensureColumns();

    VoiceProfileRepository::migrateLegacy();
    FaceEmbeddingRepository::ensureColumns();

    DbService::applyPragmas();
    if (!secure_delete::enable())
      LOG_WARN << "Deleted biometric rows may linger in free pages until they "
                  "are reused";

    if (identitySink)
      identitySink->reconcile();

    drogon::async_run([&invitationRevocation]() -> drogon::Task<void> {
      try {
        const auto revoked = co_await invitationRevocation.revokeDisabledModules();
        if (revoked > 0)
          LOG_INFO << "Identity: " << revoked << " pending invitation(s) of modules that are off were revoked";
      }
      catch (const std::exception& error) {
        LOG_WARN << "Identity: the invitations of the modules that are off could not be reconciled: " << error.what();
      }
      co_return;
    });

    if (face.enabled) {
      FaceService::instance().init();
      if (!FaceService::instance().isLoaded()) {
        LOG_WARN << "FaceService not loaded — facial login disabled";
      }
      else {
        if (FaceService::instance().initLiveness(face.livenessModelDir) !=
                AntiSpoofLoad::Loaded &&
            face.livenessRequired)
          LOG_WARN << "Face login and registration are refused until the "
                      "anti-spoofing models are provisioned "
                      "(services/identity/scripts/provision.sh)";
        drogon::async_run(
            [service = &faceUpgrade] { return upgradeFaces(service); });
      }
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

  shutdown_signal::onQuit([dbPath = identityDb.dbPath] {
    CertService::shutdown();
    DbService::freezeClient(dbPath);
  });

  CandidateRetentionService candidateRetention;
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
        ObjectDeletionWorker::instance().start();
        pairing_banner::printWhenUnpaired(listener.port);
      });

  drogon::app().setThreadNum(0).run();

  rpcDrain.stop();
  FaceService::instance().shutdown();
  SpeakerEmbeddingService::instance().shutdown();
  return 0;
}
