#include <camera/camera-config.hxx>
#include <camera/nats-camera-change-sink.hxx>
#include <config/app-config.hxx>
#include <controllers/camera-media-socket.hxx>
#include <controllers/health-controller.hxx>
#include <drogon/drogon.h>
#include <drogon/utils/coroutine.h>
#include <feature/api/camera-control/controllers/camera-control-controller.hxx>
#include <feature/api/camera/controllers/camera-controller.hxx>
#include <feature/api/zone/controllers/zone-controller.hxx>
#include <feature/actions/camera-action-rpc-service.hxx>
#include <feature/health/health-rpc-service.hxx>
#include <feature/sync/camera-sync-rpc-service.hxx>
#include <grpc-server-identity.hxx>
#include <grpcpp/grpcpp.h>
#include <filter/device/device-filter.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <filter/role/role-filter.hxx>
#include <filter/valid-json/valid-json-filter.hxx>
#include <monitor/camera-health-monitor.hxx>
#include <monitor/nats-health-event-sink.hxx>
#include <shared/services/evidence/evidence-uploader.hxx>
#include <objects/ncnn-object-detector.hxx>
#include <operator/camera-operator-service.hxx>
#include <operator/go2rtc-frame-source.hxx>
#include <operator/identity-known-person-matcher.hxx>
#include <operator/known-person-matcher.hxx>
#include <operator/nats-object-event-sink.hxx>
#include <operator/operator-config.hxx>
#include <operator/zone-provider.hxx>
#include <server/listener-config.hxx>
#include <shared/repositories/camera/camera-repository.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/room/room-manager.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/services/stream/go2rtc-manager.hxx>
#include <shared/services/stream/camera-source-registrar.hxx>
#include <shared/services/stream/stream-hub.hxx>
#include <shared/wrapper/blocking-task/blocking-task.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>
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

// Static lifetime: per-camera coroutines hold these non-owning pointers.
Go2rtcFrameSource& frameSource()
{
  static Go2rtcFrameSource source;
  return source;
}

} // namespace

int main()
{
  DbService::enableUriFilenames();

  ConfigService::load("config.toml");

  const CameraDbConfig cameraDb = CameraConfig::resolveDb();
  const ListenerConfig listener = ListenerConfig::resolve(7026);
  const GrpcListenerConfig grpcListener = GrpcListenerConfig::resolve(7036);

  CameraSyncRpcService cameraSyncRpc;
  CameraActionRpcService cameraActionRpc(
      {.callers = {argus::sdk::CallerCredential{
           .service = "argus-guard",
           .secret = ConfigService::getString("grpc.caller_guard")}},
       .transcriber = makeHttpSttTranscriber()});
  HealthRpcService healthRpc;

  grpc::ServerBuilder builder;
  const std::string grpcAddress =
      grpcListener.host + ":" + std::to_string(grpcListener.port);
  builder.AddListeningPort(grpcAddress, grpc::InsecureServerCredentials());
  builder.RegisterService(&cameraSyncRpc);
  builder.RegisterService(&cameraActionRpc);
  builder.RegisterService(&healthRpc);
  std::unique_ptr<grpc::Server> grpcServer(builder.BuildAndStart());
  if (!grpcServer) {
    LOG_FATAL << "gRPC server failed to listen on " << grpcAddress;
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
        AppConfig::applyCors(resp);
      });

  drogon::app().setExceptionHandler(AppConfig::handleException);

  drogon::app().setCustomErrorHandler(
      [](drogon::HttpStatusCode code, const drogon::HttpRequestPtr&) {
        if (code == drogon::k405MethodNotAllowed)
          return AppConfig::get405Response();
        return AppConfig::get404Response();
      });

  LOG_INFO << "Listening on " << listener.host << ":" << listener.port
           << " (plain); camera database " << cameraDb.dbPath
           << "; gRPC SyncService on " << grpcAddress;

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
    changeSink = std::make_shared<NatsCameraChangeSink>(natsBus);
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
    NatsObjectEventSink::ensureStream(natsBus, {});
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

  const bool healthEnabled = !ConfigService::hasKey("health.enabled") ||
                             ConfigService::getBool("health.enabled");
  if (healthEnabled) {
    CameraHealthConfig healthConfig;
    healthConfig.intervalMs = ConfigService::getInt("health.interval_ms");
    if (healthConfig.intervalMs <= 0)
      healthConfig.intervalMs = 60000;
    const auto threshold = [](const std::string& key, double fallback) {
      const double value = ConfigService::getDouble(key);
      return value > 0 ? value : fallback;
    };
    healthConfig.thresholds.dark = threshold("health.dark_threshold", 25.0);
    healthConfig.thresholds.bright = threshold("health.bright_threshold", 235.0);
    healthConfig.thresholds.blur = threshold("health.blur_threshold", 18.0);
    healthConfig.thresholds.sceneDiff =
        threshold("health.scene_diff", 0.35);
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

  RoomManager roomManagerLifecycle;
  roomManagerLifecycle.init();

  drogon::app().registerBeginningAdvice(
      [&cameraDb, &operatorService, &healthMonitor, &cameraActionRpc,
       &objectSink]() {
    DbService::installExtensions();

    if (!DbService::runScriptFile(cameraDb.schemaPath)) {
      LOG_FATAL << "Camera database schema failed to apply — aborting startup";
      _exit(1);
    }

    if (!cameraActionRpc.migrateActionSchema()) {
      LOG_FATAL << "Camera action schema migration failed — aborting startup";
      _exit(1);
    }

    if (objectSink)
      objectSink->reconcile();

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
          for (const auto& camera : cameras)
            cameraSourceRegistrar().apply(camera);
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

  drogon::app()
      .setThreadNum(0)
      .run();

  grpcServer->Shutdown();

  if (operatorService)
    operatorService->stop();
  if (healthMonitor)
    healthMonitor->stop();
  StreamHub::instance().shutdown();
  Go2rtcManager::instance().shutdown();
  roomManagerLifecycle.shutdown();
  return 0;
}
