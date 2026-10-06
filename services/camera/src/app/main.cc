#include <app/rpc/camera-rpc-server.hxx>
#include <config/camera-config.hxx>
#include <feature/media/camera-media-socket.hxx>
#include <feature/media/media-access-check.hxx>
#include <feature/media/session-revocation-consumer.hxx>
#include <drogon/drogon.h>
#include <shared/repositories/zone/zone-repository.hxx>
#include <drogon/utils/coroutine.h>
#include <feature/camera-control/controllers/camera-control-controller.hxx>
#include <feature/camera/controllers/camera-controller.hxx>
#include <feature/zone/controllers/zone-controller.hxx>
#include <feature/webrtc/controllers/camera-webrtc-controller.hxx>
#include <feature/webrtc/services/webrtc-session-closer.hxx>
#include <feature/actions/camera-action-rpc-service.hxx>
#include <feature/health/health-rpc-service.hxx>
#include <feature/module-data/services/camera-module-data.hxx>
#include <feature/settings/camera-settings.hxx>
#include <feature/sync/camera-sync-rpc-service.hxx>
#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/module-feed.hxx>
#include <auth/role-access.hxx>
#include <auth/role-filter.hxx>
#include <auth/valid-json-filter.hxx>
#include <http/certificate-reload.hxx>
#include <http/cors.hxx>
#include <http/error-handler.hxx>
#include <http/health-controller.hxx>
#include <http/listener-config.hxx>
#include <http/route-announcements.hxx>
#include <mdns/mdns-service.hxx>
#include <feature/monitor/camera-health-monitor.hxx>
#include <feature/monitor/camera-presence.hxx>
#include <feature/monitor/nats-health-event-sink.hxx>
#include <shared/services/event-stream/event-stream.hxx>
#include <feature/operator/services/evidence/evidence-uploader.hxx>
#include <feature/objects/ncnn-object-detector.hxx>
#include <feature/operator/camera-operator-service.hxx>
#include <shared/services/stream/go2rtc-frame-source.hxx>
#include <feature/operator/identity-known-person-matcher.hxx>
#include <feature/operator/known-person-matcher.hxx>
#include <feature/operator/nats-object-event-sink.hxx>
#include <config/operator-config.hxx>
#include <feature/operator/zone-provider.hxx>
#include <shared/repositories/camera/camera-repository.hxx>
#include <config/config-service.hxx>
#include <shared/services/change-sink/nats-camera-change-sink.hxx>
#include <sqlite/db-service.hxx>
#include <shared/services/stream/go2rtc-manager.hxx>
#include <shared/services/stream/camera-source-registrar.hxx>
#include <feature/camera/services/camera-feature-service.hxx>
#include <shared/services/privacy/camera-audio-policy.hxx>
#include <shared/services/secret-box/secret-box.hxx>
#include <shared/services/stream/stream-hub.hxx>
#include <runtime/blocking-task.hxx>
#include <settings/settings-rpc.hxx>
#include <runtime/shutdown-signal.hxx>
#include <runtime/log-output.hxx>
#include <nats/nats-bus.hxx>
#include <unistd.h>

#include <json/value.h>
#include <memory>
#include <string>
#include <vector>

namespace
{
constexpr double kAudioPolicyRefreshSeconds = 15.0;
constexpr double kMediaAccessCheckSeconds = 60.0;

Json::Value drogonConfig(const CameraDbConfig& cameraDb,
                         const ListenerConfig& listener)
{
  Json::Value config = ConfigService::drogonConfig();
  if (config.isNull())
    config = Json::Value(Json::objectValue);

  Json::Value clients(Json::arrayValue);
  Json::Value client(Json::objectValue);
  client["name"] = "default";
  client["rdbms"] = "sqlite3";
  client["filename"] = cameraDb.dbPath;
  client["is_fast"] = false;
  client["number_of_connections"] = 1;
  client["timeout"] = -1.0;
  clients.append(client);
  config["db_clients"] = std::move(clients);

  config["listeners"] = listenerJson(listener);

  return config;
}

Go2rtcFrameSource& frameSource()
{
  static Go2rtcFrameSource source;
  return source;
}

drogon::Task<void> applyInitialSources()
{
  try {
    CameraRepository repository;
    auto cameras = co_await repository.findEnabled();
    if (cameras.empty())
      co_return;
    co_await BlockingTask<void>([cameras = std::move(cameras)] {
      cameraSourceRegistrar().applyAll(cameras);
    });
  }
  catch (const std::exception& error) {
    LOG_WARN << "Camera source registrar: initial apply failed: " << error.what();
  }
  catch (...) {
    LOG_WARN << "Camera source registrar: initial apply failed with unknown error";
  }
}

drogon::Task<void> reconcileCapabilities()
{
  try {
    const CameraFeatureService cameras;
    if (const int updated = co_await cameras.reconcileCapabilities(); updated > 0)
      LOG_INFO << "Camera capabilities: refreshed " << updated << " camera(s)";
  }
  catch (const std::exception& error) {
    LOG_WARN << "Camera capabilities: reconcile failed: " << error.what();
  }
}

drogon::Task<void> startAfterSources(CameraOperatorService* operatorService,
                                     CameraHealthMonitor* healthMonitor)
{
  co_await BlockingTask<void>([] { Go2rtcManager::instance().start(); });
  co_await applyInitialSources();
  co_await reconcileCapabilities();
  co_await drogon::switchThreadCoro(drogon::app().getLoop());
  if (operatorService)
    operatorService->start();
  if (healthMonitor)
    healthMonitor->start();
}

}

int main()
{
  log_output::flushEachLine();
  DbService::enableUriFilenames();

  ConfigService::load("config.toml");

  const CameraDbConfig cameraDb = CameraConfig::resolveDb();
  const ListenerConfig listener = CameraConfig::resolveListener();

  try {
    DeviceFilter::requireFingerprintSecret();
  }
  catch (const std::exception& error) {
    LOG_FATAL << error.what() << " — aborting startup";
    return 1;
  }
  bool keyCreated = false;
  switch (secret_box::loadOrCreateKey(cameraDb.secretKeyPath)) {
    case secret_box::KeyFileResult::Loaded:
      break;
    case secret_box::KeyFileResult::Created:
      keyCreated = true;
      LOG_INFO << "Camera secrets: created the instance key at " << cameraDb.secretKeyPath;
      break;
    case secret_box::KeyFileResult::Failed:
      LOG_FATAL << "Camera secrets: cannot read or create the key at "
                << cameraDb.secretKeyPath << " — aborting startup";
      return 1;
  }

  CameraSyncRpcService cameraSyncRpc(
      {argus::client::CallerCredential{
           .service = "argus-sync",
           .secret = CameraConfig::resolveSyncCallerSecret()},
       argus::client::CallerCredential{
           .service = "argus-llm",
           .secret = CameraConfig::resolveLlmCallerSecret()}});
  CameraActionRpcService cameraActionRpc(
      {.callers = {argus::client::CallerCredential{
           .service = "argus-guard",
           .secret = CameraConfig::resolveGuardCallerSecret()}},
       .transcriber = makeHttpSttTranscriber()});
  HealthRpcService healthRpc;
  SettingsRegistry settings(cameraSettingsCatalog());
  settings.onChange([](const std::vector<std::string>&) {
    StreamHub::instance().refreshViewerLimits();
  });
  std::vector<grpc::Service*> rpcServices{&cameraSyncRpc, &cameraActionRpc,
                                          &healthRpc};
  std::unique_ptr<SettingsRpcService> settingsRpc;
  CameraModuleData moduleData;
  if (auto callers = CameraConfig::resolveSettingsCallers(); !callers.empty()) {
    settingsRpc = std::make_unique<SettingsRpcService>(
        SettingsRpcInput{.service = "camera",
                         .registry = &settings,
                         .credentials = std::move(callers)});
    settingsRpc->attachModuleData(moduleData);
    rpcServices.push_back(settingsRpc.get());
  }

  CameraRpcServer rpc({.services = std::move(rpcServices)});
  if (!rpc.listening()) {
    LOG_FATAL << "gRPC server failed to listen on " << rpc.address();
    return 1;
  }

  drogon::app().registerController(std::make_shared<CameraController>());
  drogon::app().registerController(std::make_shared<ZoneController>());
  drogon::app().registerController(std::make_shared<CameraControlController>());
  drogon::app().registerController(std::make_shared<CameraWebRtcController>());

  drogon::app().registerFilter(std::make_shared<DeviceFilter>());
  drogon::app().registerFilter(std::make_shared<ValidJsonFilter>());
  drogon::app().registerFilter(std::make_shared<JwtFilter>());
  drogon::app().registerFilter(std::make_shared<RoleFilter>());

  MediaSessionRegistry mediaSessions;
  CameraTalkService talkService(
      {}, [] { return moduleGate().enabled(role_access::kSurveillanceModule); });
  MediaAccessCheck mediaAccess(MediaAccessCheck::remote());
  drogon::app().registerController(std::make_shared<CameraMediaSocket>(
      CameraMediaSocket::Dependencies{
          .sessions = mediaSessions, .talk = talkService, .access = mediaAccess}));

  drogon::app().loadConfigJson(drogonConfig(cameraDb, listener));
  certificate_reload::watch(listener);

  drogon::app().registerPostHandlingAdvice(
      [](const drogon::HttpRequestPtr&, const drogon::HttpResponsePtr& resp) {
        Cors::apply(resp);
      });

  drogon::app().setExceptionHandler(ErrorHandler::handleException);

  drogon::app().setCustomErrorHandler(
      [](drogon::HttpStatusCode code, const drogon::HttpRequestPtr&) {
        return ErrorHandler::unmatchedRoute(code);
      });

  LOG_INFO << "Listening on " << listener.host << ":" << listener.port
           << (listener.tls ? " (TLS" : " (plain") << ", cert "
           << listener.certPath << "); camera database " << cameraDb.dbPath
           << "; gRPC SyncService on " << rpc.address();

  std::shared_ptr<NatsCameraChangeSink> changeSink;
  std::shared_ptr<NatsBus> natsBus;
  std::unique_ptr<NatsObjectEventSink> objectSink;
  const std::string natsUrl = ConfigService::getString("nats.url");
  if (natsUrl.empty()) {
    LOG_INFO << "NATS not configured; camera change funnel disabled";
  }
  else {
    natsBus = std::make_shared<NatsBus>();
    if (natsBus->connect())
      LOG_INFO << "NATS event bus connected to " << natsBus->options().url;
    else
      LOG_WARN << "NATS unavailable at " << natsUrl
               << "; bus reconnects in background, events retained locally";
    changeSink = std::make_shared<NatsCameraChangeSink>(
        natsBus, NatsCameraChangeSink::Config{});
    camera_change::setSink(changeSink.get());
    const int cooldownMs = ConfigService::getInt("operator.cooldown_ms");
    const int maxPending = ConfigService::getInt("operator.outbox_max_pending");
    objectSink = std::make_unique<NatsObjectEventSink>(
        natsBus, NatsObjectEventSink::Config{
                     .cooldownMs = cooldownMs > 0 ? cooldownMs : 30000,
                     .maxPending = maxPending > 0 ? maxPending : 5000,
                     .retryMs = 500,
                     .sessionTag = {},
                     .streamName = {},
                     .publishSubject = {}});
    static_cast<void>(camera_event_stream::ensure(natsBus, {}));
  }
  const WebRtcSessionCloser webrtcSessions{};
  SessionRevocationConsumer sessionRevocations(
      {.bus = natsBus,
       .sessions = &mediaSessions,
       .onRevoked =
           [&webrtcSessions](const MediaSessionKey& session) {
             drogon::app().getLoop()->queueInLoop(
                 [closer = &webrtcSessions,
                  viewer = WebRtcViewer{.userId = session.userId, .sessionId = session.sessionId}]() {
                   drogon::async_run([closer, viewer]() { return closer->closeViewer(viewer); });
                 });
           },
       .onUserChanged =
           [&webrtcSessions](int64_t userId) {
             drogon::app().getLoop()->queueInLoop([closer = &webrtcSessions, userId]() {
               drogon::async_run([closer, userId]() { return closer->closeUser(userId); });
             });
           }},
      SessionRevocationConsumer::defaults());

  const auto modules = module_gate::install({.service = "camera", .bus = natsBus});
  const auto surveillanceActive = [] {
    return moduleGate().enabled(role_access::kSurveillanceModule);
  };
  moduleGate().onChange([&mediaSessions, &webrtcSessions](const ModuleChange& change) {
    if (change.id != role_access::kSurveillanceModule || change.enabled)
      return;
    static_cast<void>(mediaSessions.closeAll("module_disabled"));
    drogon::app().getLoop()->queueInLoop([closer = &webrtcSessions]() {
      drogon::async_run([closer]() { return closer->closeAll(); });
    });
  });

  const ObjectsConfig objectsConfig = operator_config::resolveObjects();
  std::unique_ptr<ObjectDetectorService> detector;
  std::unique_ptr<CameraOperatorService> operatorService;
  std::unique_ptr<ZoneProvider> zoneProvider;
  std::unique_ptr<IdentityKnownPersonMatcher> identityMatcher;
  CameraPresenceRecorder presenceRecorder;
  std::unique_ptr<NatsHealthEventSink> healthSink;
  std::unique_ptr<CameraHealthMonitor> healthMonitor;
  if (objectsConfig.enabled) {
    ObjectDetectorOptions detectorOptions;
    detectorOptions.modelDir = objectsConfig.model;
    detectorOptions.classes = objectsConfig.classes;
    detectorOptions.inputSize = objectsConfig.inputSize;
    detectorOptions.confidence = objectsConfig.confidence;
    detectorOptions.useVulkan = objectsConfig.useVulkan;
    detector = std::make_unique<ObjectDetectorService>(detectorOptions);
    detector->init();
    if (detector->isLoaded()) {
      LOG_INFO << "Object detector backend: " << detector->backend();
      CameraOperatorService::Inputs inputs;
    static_cast<void>(talkService.stopAll("module_disabled"));
      inputs.dependencies.detector = detector.get();
      inputs.dependencies.source = &frameSource();
      inputs.dependencies.sink = objectSink.get();
      if (!inputs.dependencies.sink)
        LOG_WARN << "NATS unavailable; object_detected events dropped";
      static NoKnownPersonMatcher noKnownPersonMatcher;
      const IdentityConfig identityConfig = operator_config::resolveIdentity();
      if (identityConfig.identify && !identityConfig.target.empty()) {
        identityMatcher =
            std::make_unique<IdentityKnownPersonMatcher>(identityConfig);
        inputs.dependencies.matcher = identityMatcher.get();
        LOG_INFO << "Camera operator: identity matcher enabled ("
                 << identityConfig.target << ")";
      }
      else {
        inputs.dependencies.matcher = &noKnownPersonMatcher;
      }
      inputs.dependencies.active = surveillanceActive;
      inputs.objects = objectsConfig;
      inputs.operator_ = operator_config::resolveOperator();
      if (inputs.operator_.zonesFromDb) {
        zoneProvider = std::make_unique<ZoneProvider>(
            inputs.operator_.zonesRefreshMs,
            std::make_unique<StaticZoneSource>(inputs.operator_.zones));
        inputs.dependencies.zones = zoneProvider.get();
      }
      operatorService = std::make_unique<CameraOperatorService>(inputs);
    }
  }
  else {
    LOG_INFO << "Object detection disabled by configuration";
  }

  const CameraHealthConfig healthConfig = CameraConfig::resolveHealth();
  if (healthConfig.enabled) {
    if (natsBus)
      healthSink = std::make_unique<NatsHealthEventSink>(natsBus);
    healthMonitor = std::make_unique<CameraHealthMonitor>(
        CameraHealthMonitor::Dependencies{.source = &frameSource(),
                                           .sink = healthSink.get(),
                                           .presence = &presenceRecorder,
                                           .active = surveillanceActive},
        healthConfig);
  }

  NatsObjectEventSink* sinkHealth = objectSink.get();
  drogon::app().registerController(std::make_shared<HealthController>(
      HealthStatus{.serviceName = "argus-camera",
                   .extras = {{"objectEvents",
                               [sinkHealth]() {
                                 if (sinkHealth == nullptr)
                                   return Json::Value(Json::objectValue);
                                 return sinkHealth->health();
                               }}}}));

  drogon::app().registerBeginningAdvice(
      [&cameraDb, &operatorService, &healthMonitor, &cameraActionRpc,
       &objectSink, &changeSink, keyCreated]() {
    DbService::installExtensions();

    if (!DbService::runScriptFile(cameraDb.schemaPath)) {
      LOG_FATAL << "Camera database schema failed to apply — aborting startup";
      _exit(1);
    }

    ZoneRepository::acceptPrivacyZones();
    CameraRepository::acceptTapoTrust();
    if (const int64_t unreadable = CameraRepository::unreadableSecrets(); unreadable > 0) {
      if (keyCreated)
        ::unlink(cameraDb.secretKeyPath.c_str());
      LOG_FATAL << "Camera secrets: " << unreadable
                << " camera(s) hold passwords sealed with another key than "
                << cameraDb.secretKeyPath
                << "; restore that key file (or point [camera] secret_key at it) — aborting startup";
      _exit(1);
    }
    if (const int64_t sealed = CameraRepository::sealPlaintextSecrets(); sealed > 0)
      LOG_INFO << "Camera secrets: encrypted the stored passwords of " << sealed
               << " camera(s)";

    if (!cameraActionRpc.migrateActionSchema()) {
      LOG_FATAL << "Camera action schema migration failed — aborting startup";
      _exit(1);
    }

    if (!NatsCameraChangeSink::repository().migrateSchema()) {
      LOG_FATAL << "Camera change outbox migration failed — aborting startup";
      _exit(1);
    }

    if (objectSink) {
      objectSink->reconcile();
    }
    if (changeSink) {
      changeSink->reconcile();
    }

    DbService::applyPragmas();

    cameraActionRpc.startLeaseSweeper();
    EvidenceUploader::instance().scheduleRetentionSweep();

    Go2rtcManager::instance().configure();
    StreamHub::instance().init();

    drogon::async_run([operator_ = operatorService.get(),
                       monitor = healthMonitor.get()]() {
      return startAfterSources(operator_, monitor);
    });
  });

  shutdown_signal::onQuit(
      [dbPath = cameraDb.dbPath] { DbService::freezeClient(dbPath); });
  if (objectSink) {
    shutdown_signal::onStop(
        shutdown_signal::drainOf(*objectSink, "camera-object-event"));
  }
  if (changeSink) {
    shutdown_signal::onStop(
        shutdown_signal::drainOf(*changeSink, "camera-change"));
  }
  if (operatorService) {
    shutdown_signal::onStop(
        shutdown_signal::drainOf(*operatorService, "camera-operator"));
  }
  if (healthMonitor) {
    shutdown_signal::onStop(
        shutdown_signal::drainOf(*healthMonitor, "camera-health"));
  }
  shutdown_signal::onStop(shutdown_signal::drainOf(talkService, "camera-talk"));
  shutdown_signal::onStop(shutdown_signal::drainOf(rpc, "camera-rpc"));
  shutdown_signal::onStop(shutdown_signal::drainOf(cameraActionRpc, "camera-actions"));
  shutdown_signal::onStop(
      shutdown_signal::drainOf(EvidenceUploader::instance(), "camera-evidence"));
  shutdown_signal::onStop(shutdown_signal::drainOf(mediaAccess, "camera-media-access"));
  drogon::app().registerBeginningAdvice(
      [&mediaAccess]() { mediaAccess.start(kMediaAccessCheckSeconds); });
  if (natsBus) {
    shutdown_signal::onStop(
        shutdown_signal::drainOf(sessionRevocations, "camera-session-revocations"));
    drogon::app().registerBeginningAdvice(
        [&sessionRevocations]() { sessionRevocations.start(); });
  }

  const IdentityConfig privacyIdentity = operator_config::resolveIdentity();
  std::shared_ptr<const IdentityClient> privacyClient;
  if (!privacyIdentity.target.empty())
    privacyClient = std::make_shared<IdentityClient>(
        privacyIdentity.target,
        argus::client::PeerCredential{.credential = privacyIdentity.credential,
                                      .fleetSecret = privacyIdentity.rpcSecret});
  else
    LOG_WARN << "Camera audio withheld: no identity target to read the "
                "household's privacy choices from";
  CameraAudioPolicy::instance().onChange([&webrtcSessions](bool) {
    StreamHub::instance().restartUpstreams();
    drogon::app().getLoop()->queueInLoop([closer = &webrtcSessions]() {
      drogon::async_run([closer]() { return closer->closeAll(); });
    });
  });
  drogon::app().registerBeginningAdvice([privacyClient]() {
    drogon::async_run([privacyClient]() {
      return CameraAudioPolicy::instance().refreshFrom(privacyClient);
    });
    drogon::app().getLoop()->runEvery(kAudioPolicyRefreshSeconds, [privacyClient]() {
      drogon::async_run([privacyClient]() {
        return CameraAudioPolicy::instance().refreshFrom(privacyClient);
      });
    });
  });

  std::unique_ptr<MdnsService> mdnsService;
  drogon::app().registerBeginningAdvice([&mdnsService, &listener]() {
    mdnsService = std::make_unique<MdnsService>(
        routeAnnouncements({.port = listener.port, .tls = listener.tls}));
    mdnsService->initialize();
  });

  drogon::app()
      .setThreadNum(0)
      .run();

  rpc.shutdown();

  if (operatorService)
    operatorService->requestStop();
  if (healthMonitor)
    healthMonitor->requestStop();
  StreamHub::instance().shutdown();
  Go2rtcManager::instance().shutdown();
  return 0;
}
