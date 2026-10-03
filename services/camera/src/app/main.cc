#include <app/rpc/camera-rpc-server.hxx>
#include <config/camera-config.hxx>
#include <feature/media/camera-media-socket.hxx>
#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <feature/camera-control/controllers/camera-control-controller.hxx>
#include <feature/camera/controllers/camera-controller.hxx>
#include <feature/zone/controllers/zone-controller.hxx>
#include <feature/actions/camera-action-rpc-service.hxx>
#include <feature/health/health-rpc-service.hxx>
#include <feature/sync/camera-sync-rpc-service.hxx>
#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/role-filter.hxx>
#include <auth/valid-json-filter.hxx>
#include <http/cors.hxx>
#include <http/error-handler.hxx>
#include <http/health-controller.hxx>
#include <http/listener-config.hxx>
#include <http/route-announcements.hxx>
#include <mdns/mdns-service.hxx>
#include <feature/monitor/camera-health-monitor.hxx>
#include <feature/monitor/nats-health-event-sink.hxx>
#include <shared/services/event-stream/event-stream.hxx>
#include <feature/operator/services/evidence/evidence-uploader.hxx>
#include <feature/objects/ncnn-object-detector.hxx>
#include <feature/operator/camera-operator-service.hxx>
#include <feature/operator/go2rtc-frame-source.hxx>
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
#include <shared/services/stream/stream-hub.hxx>
#include <runtime/blocking-task.hxx>
#include <runtime/shutdown-signal.hxx>
#include <runtime/log-output.hxx>
#include <nats/nats-bus.hxx>
#include <unistd.h>

#include <json/value.h>
#include <memory>
#include <string>

namespace
{

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
  config["db_clients"] = clients;

  config["listeners"] = listenerJson(listener);

  return config;
}

Go2rtcFrameSource& frameSource()
{
  static Go2rtcFrameSource source;
  return source;
}

}

int main()
{
  log_output::flushEachLine();
  DbService::enableUriFilenames();

  ConfigService::load("config.toml");

  const CameraDbConfig cameraDb = CameraConfig::resolveDb();
  const ListenerConfig listener = CameraConfig::resolveListener();

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

  CameraRpcServer rpc(
      {.services = {&cameraSyncRpc, &cameraActionRpc, &healthRpc}});
  if (!rpc.listening()) {
    LOG_FATAL << "gRPC server failed to listen on " << rpc.address();
    return 1;
  }

  drogon::app().registerController(std::make_shared<CameraController>());
  drogon::app().registerController(std::make_shared<ZoneController>());
  drogon::app().registerController(std::make_shared<CameraControlController>());

  drogon::app().registerFilter(std::make_shared<DeviceFilter>());
  drogon::app().registerFilter(std::make_shared<ValidJsonFilter>());
  drogon::app().registerFilter(std::make_shared<JwtFilter>());
  drogon::app().registerFilter(std::make_shared<RoleFilter>());

  drogon::app().registerController(
      std::make_shared<CameraMediaSocket>());

  drogon::app().loadConfigJson(drogonConfig(cameraDb, listener));

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

  const ObjectsConfig objectsConfig = operator_config::resolveObjects();
  std::unique_ptr<ObjectDetectorService> detector;
  std::unique_ptr<CameraOperatorService> operatorService;
  std::unique_ptr<ZoneProvider> zoneProvider;
  std::unique_ptr<IdentityKnownPersonMatcher> identityMatcher;
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
                                           .sink = healthSink.get()},
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
       &objectSink, &changeSink]() {
    DbService::installExtensions();

    if (!DbService::runScriptFile(cameraDb.schemaPath)) {
      LOG_FATAL << "Camera database schema failed to apply — aborting startup";
      _exit(1);
    }

    if (!cameraActionRpc.migrateActionSchema()) {
      LOG_FATAL << "Camera action schema migration failed — aborting startup";
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

    Go2rtcManager::instance().init();
    StreamHub::instance().init();

    if (operatorService)
      operatorService->start();
    if (healthMonitor)
      healthMonitor->start();

    drogon::async_run([]() -> drogon::Task<void> {
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
        LOG_WARN << "Camera source registrar: initial apply failed: "
                 << error.what();
      }
      catch (...) {
        LOG_WARN << "Camera source registrar: initial apply failed with "
                    "unknown error";
      }
      co_return;
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
