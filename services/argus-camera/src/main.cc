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
#include <feature/health/health-rpc-service.hxx>
#include <feature/sync/camera-sync-rpc-service.hxx>
#include <grpcpp/grpcpp.h>
#include <filter/device/device-filter.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <filter/role/role-filter.hxx>
#include <filter/valid-json/valid-json-filter.hxx>
#include <objects/ncnn-object-detector.hxx>
#include <operator/camera-operator-service.hxx>
#include <operator/go2rtc-frame-source.hxx>
#include <operator/known-person-matcher.hxx>
#include <operator/nats-object-event-sink.hxx>
#include <operator/operator-config.hxx>
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

NatsObjectEventSink& objectEventSink(const std::shared_ptr<NatsBus>& bus)
{
  static NatsObjectEventSink sink(bus);
  return sink;
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
  HealthRpcService healthRpc;

  grpc::ServerBuilder builder;
  const std::string grpcAddress =
      grpcListener.host + ":" + std::to_string(grpcListener.port);
  builder.AddListeningPort(grpcAddress, grpc::InsecureServerCredentials());
  builder.RegisterService(&cameraSyncRpc);
  builder.RegisterService(&healthRpc);
  std::unique_ptr<grpc::Server> grpcServer(builder.BuildAndStart());
  if (!grpcServer) {
    LOG_FATAL << "gRPC server failed to listen on " << grpcAddress;
    return 1;
  }

  drogon::app().registerController(std::make_shared<HealthController>(HealthStatus{.serviceName = "argus-camera", .extras = {}}));
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
  const std::string natsUrl = ConfigService::getString("nats.url");
  if (natsUrl.empty()) {
    LOG_INFO << "NATS not configured; camera change funnel disabled";
  }
  else {
    natsBus = std::make_shared<NatsBus>();
    if (natsBus->connect()) {
      LOG_INFO << "NATS event bus connected to " << natsBus->options().url;
      changeSink = std::make_shared<NatsCameraChangeSink>(natsBus);
      camera_change::setSink(changeSink.get());
      NatsObjectEventSink::ensureStream(natsUrl);
    }
    else {
      natsBus.reset();
      LOG_WARN << "NATS unavailable at " << natsUrl
               << "; camera change funnel disabled";
    }
  }

  const ObjectsConfig objectsConfig = operator_config::resolveObjects();
  std::unique_ptr<ObjectDetectorService> detector;
  std::unique_ptr<CameraOperatorService> operatorService;
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
      static NoKnownPersonMatcher noKnownPersonMatcher;
      CameraOperatorService::Inputs inputs;
      inputs.dependencies.detector = detector.get();
      inputs.dependencies.source = &frameSource();
      inputs.dependencies.sink = natsBus
                                     ? static_cast<IObjectEventSink*>(
                                           &objectEventSink(natsBus))
                                     : nullptr;
      if (!inputs.dependencies.sink)
        LOG_WARN << "NATS unavailable; object_detected events dropped";
      inputs.dependencies.matcher = &noKnownPersonMatcher;
      inputs.objects = objectsConfig;
      inputs.operator_ = operator_config::resolveOperator();
      operatorService = std::make_unique<CameraOperatorService>(inputs);
    }
  }
  else {
    LOG_INFO << "Object detection disabled by configuration";
  }

  RoomManager roomManagerLifecycle;
  roomManagerLifecycle.init();

  drogon::app().registerBeginningAdvice([&cameraDb, &operatorService]() {
    DbService::installExtensions();

    if (!DbService::runScriptFile(cameraDb.schemaPath)) {
      LOG_FATAL << "Camera database schema failed to apply — aborting startup";
      _exit(1);
    }

    DbService::applyPragmas();

    Go2rtcManager::instance().init();
    StreamHub::instance().init();

    if (operatorService)
      operatorService->start();

    drogon::async_run([]() -> drogon::Task<void> {
      CameraRepository repository;
      auto cameras = co_await repository.findEnabled();
      if (cameras.empty())
        co_return;
      co_await BlockingTask<void>([cameras = std::move(cameras)] {
        for (const auto& camera : cameras)
          cameraSourceRegistrar().apply(camera);
      });
      co_return;
    });
  });

  drogon::app()
      .setThreadNum(0)
      .run();

  grpcServer->Shutdown();

  if (operatorService)
    operatorService->stop();
  StreamHub::instance().shutdown();
  Go2rtcManager::instance().shutdown();
  roomManagerLifecycle.shutdown();
  return 0;
}
