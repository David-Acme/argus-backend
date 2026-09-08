#include <camera/camera-config.hxx>
#include <camera/nats-camera-change-sink.hxx>
#include <config/app-config.hxx>
#include <controllers/camera-media-service.hxx>
#include <controllers/health-controller.hxx>
#include <drogon/drogon.h>
#include <feature/api/camera-control/controllers/camera-control-controller.hxx>
#include <feature/api/camera/controllers/camera-controller.hxx>
#include <feature/api/zone/controllers/zone-controller.hxx>
#include <feature/health/health-rpc-service.hxx>
#include <feature/socket/sync/socket/sync-socket.hxx>
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
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/room/room-manager.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/services/stream/go2rtc-manager.hxx>
#include <shared/services/stream/stream-hub.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>
#include <unistd.h>

#include <json/value.h>
#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

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

// Config-gated read-only identity client ([identity] db): the sync socket
// resolves the caller's user row and serves the user table from it. The
// filters do not read here — they validate over the identity RPC.
void installIdentityClient()
{
  const auto path = ConfigService::getString("identity.db");
  if (path.empty())
    return;

  for (int ms = 0; ms < 30000 && !std::filesystem::exists(path); ms += 250)
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

  DbService::enableUriFilenames();
  try {
    const auto identity = drogon::orm::DbClient::newSqlite3Client(
        "filename=file:" + path + "?mode=ro", 1);
    identity->execSqlSync("PRAGMA busy_timeout = 5000");
    DbService::setIdentityClient(identity);
    LOG_INFO << "Identity database opened read-only: " << path;
  }
  catch (const std::exception& e) {
    LOG_WARN << "Identity database open failed (" << e.what()
             << "); identity reads fall back to the default client";
  }
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

  installIdentityClient();

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

  drogon::app().registerController(std::make_shared<HealthController>(HealthStatus{.serviceName = "argus-camera"}));
  drogon::app().registerController(std::make_shared<CameraController>());
  drogon::app().registerController(std::make_shared<ZoneController>());
  drogon::app().registerController(std::make_shared<CameraControlController>());

  drogon::app().registerFilter(std::make_shared<DeviceFilter>());
  drogon::app().registerFilter(std::make_shared<ValidJsonFilter>());
  drogon::app().registerFilter(std::make_shared<JwtFilter>());
  drogon::app().registerFilter(std::make_shared<RoleFilter>());

  const auto syncSocket = std::make_shared<SyncSocket>();
  syncSocket->setForwarder(std::make_shared<CameraMediaService>());
  drogon::app().registerController(syncSocket);

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
