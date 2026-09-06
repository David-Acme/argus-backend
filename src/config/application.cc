#include "application.hxx"

#include <cctype>
#include <config/app-config.hxx>
#include <csignal>
#include <drogon/drogon.h>
#include <execinfo.h>
#include <feature/api/auth/controllers/auth-controller.hxx>
#include <feature/api/camera/controllers/camera-controller.hxx>
#include <feature/api/camera/services/socket-camera-change-sink.hxx>
#include <shared/services/socket/socket-user-change-sink.hxx>
#include <feature/api/invitation/controllers/invitation-controller.hxx>
#include <feature/api/pairing/controllers/pairing-controller.hxx>
#include <feature/api/user/controllers/portrait-preview-controller.hxx>
#include <feature/api/user/controllers/user-controller.hxx>
#include <feature/api/zone/controllers/zone-controller.hxx>
#include <feature/socket/sync/media/sync-media-service.hxx>
#include <feature/socket/sync/socket/sync-socket.hxx>
#include <filter/device/device-filter.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <filter/role/role-filter.hxx>
#include <filter/valid-json/valid-json-filter.hxx>
#include <iostream>
#include <llama.h>
#include <memory>
#include <shared/services/cert/adapter/cert-service-adapter.hxx>
#include <shared/services/cert/cert-service.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/extract/adapter/extraction-service-adapter.hxx>
#include <shared/services/face/adapter/face-service-adapter.hxx>
#include <shared/services/face/face-db.hxx>
#include <shared/services/face/face-service.hxx>
#include <shared/services/intent/adapter/intent-service-adapter.hxx>
#include <shared/services/llm/adapter/llm-service-adapter.hxx>
#include <shared/services/mdns/adapter/mdns-service-adapter.hxx>
#include <shared/services/memory/adapter/memory-service-adapter.hxx>
#include <shared/services/queue/adapter/queue-manager-service-adapter.hxx>
#include <shared/services/room/adapter/room-manager-service-adapter.hxx>
#include <shared/services/socket/socket-service.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/services/stream/go2rtc-manager.hxx>
#include <shared/services/stream/media-relay.hxx>
#include <shared/utils/json-util/json-util.hxx>
#include <shared/services/stream/stream-hub.hxx>
#include <shared/services/stt/adapter/stt-service-adapter.hxx>
#include <shared/services/tts/adapter/tts-service-adapter.hxx>
#include <shared/services/vision/adapter/vision-service-adapter.hxx>
#include <shared/wrapper/qr/qr-render.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>
#include <unistd.h>

using namespace drogon;

namespace
{

void crashHandler(int sig)
{
  void* array[32];
  int size = backtrace(array, 32);
  LOG_FATAL << "======= CRASH (signal " << sig << ") BACKTRACE =======";
  backtrace_symbols_fd(array, size, STDERR_FILENO);
  _exit(128 + sig);
}

void forceShutdownHandler(int)
{
  static int sigCount = 0;
  sigCount++;

  if (sigCount == 1) {
    drogon::app().quit();
    return;
  }

  _exit(128 + SIGINT);
}

void registerIdentitySurface()
{
  app().registerFilter(std::make_shared<DeviceFilter>());
  app().registerFilter(std::make_shared<ValidJsonFilter>());
  app().registerFilter(std::make_shared<JwtFilter>());
  app().registerFilter(std::make_shared<RoleFilter>());

  app().registerController(std::make_shared<AuthController>());
  app().registerController(std::make_shared<InvitationController>());
  app().registerController(std::make_shared<PairingController>());
  app().registerController(std::make_shared<UserController>());
  app().registerController(std::make_shared<PortraitPreviewController>());

  const auto syncSocket = std::make_shared<SyncSocket>();
  syncSocket->setForwarder(std::make_shared<SyncMediaService>());
  app().registerController(syncSocket);
}

void registerCameraSurface()
{
  // The camera controllers live in the shared static library, so their
  // AutoCreation registration is linker-dropped: they register explicitly
  // here and in argus-camera.
  app().registerController(std::make_shared<CameraController>());
  app().registerController(std::make_shared<ZoneController>());
}

void printPairingBanner()
{
  const std::string code = CertService::pairingCode();
  if (code.empty())
    return;

  std::string host = ConfigService::getString("mdns.name");
  if (host.empty())
    host = "Argus";
  for (char& c : host)
    c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  host += ".local";
  const int port = ConfigService::getInt("mdns.port");
  const std::string scheme = "https";
  const std::string serviceType = ConfigService::getString("mdns.service_type");

  Json::Value payload(Json::objectValue);
  payload["host"] = host;
  payload["port"] = port;
  payload["scheme"] = scheme;
  payload["code"] = code;
  payload["instanceId"] = CertService::instanceId();
  payload["caFingerprint"] = CertService::caFingerprint();
  payload["serverFingerprint"] = CertService::serverFingerprint();
  payload["serviceType"] = serviceType;
  Json::Value txt(Json::objectValue);
  for (const auto& [key, value] : ConfigService::getStringPairs("mdns.txt"))
    txt[key] = value;
  payload["txt"] = txt;

  std::cout << "\n"
            << "============================================================\n"
            << "  ARGUS — pairing required\n"
            << "\n"
            << qr_render::asciiQr(json_util::toString(payload)) << "\n"
            << "  Scan the QR code with the Argus app to pair this server.\n"
            << "\n"
            << "  Server:    " << scheme << "://" << host << ":" << port << "\n"
            << "  Host:      " << host << "\n"
            << "  Port:      " << port << "\n"
            << "  Scheme:    " << scheme << "\n"
            << "  Code:      " << code << "\n"
            << "  Instance:  " << CertService::instanceId() << "\n"
            << "  CA:        " << CertService::caFingerprint() << "\n"
            << "  Server FP: " << CertService::serverFingerprint() << "\n"
            << "  mDNS:      " << serviceType << "\n";
  for (const auto& [key, value] : ConfigService::getStringPairs("mdns.txt"))
    std::cout << "             " << key << "=" << value << "\n";
  std::cout << "============================================================\n"
            << std::flush;
}

void installEventBus()
{
  if (ConfigService::getString("nats.url").empty()) {
    LOG_INFO << "NATS not configured; sync-change fan-out disabled";
    return;
  }

  const auto bus = std::make_shared<NatsBus>();
  if (!bus->connect()) {
    LOG_WARN << "NATS unavailable at " << bus->options().url
             << "; continuing without sync-change fan-out";
    return;
  }
  SocketService::setEventBus(bus);
  LOG_INFO << "NATS event bus connected to " << bus->options().url;
}

void installIdentityClient()
{
  // Transitional cutover read (F1-5): with [identity] db configured the
  // legacy JwtFilter validates refresh tokens and user rows against the
  // gateway-minted identity database instead of argus.db. Without the key
  // the fallback to the default client keeps the pre-cutover behavior
  // byte-identical. URI filenames must be enabled before the first
  // sqlite3_open, so this runs before loadConfigJson.
  const auto path = ConfigService::getString("identity.db");
  if (path.empty())
    return;

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
             << "); auth reads fall back to the default client";
  }
}

void installUserChangeSink()
{
  // The productivity and notification feature services route their change
  // events through the sink; the legacy binds the local SocketService plus
  // SyncAuditService, exactly the pre-cutover path.
  static const SocketUserChangeSink sink;
  user_change::setProductivitySink(&sink);
  user_change::setNotificationSink(&sink);
}

void installCameraSurface()
{
  // The camera feature services route their change events through the sink;
  // the legacy binds the local SocketService plus SyncAuditService, exactly
  // the pre-cutover path.
  static const SocketCameraChangeSink sink;
  camera_change::setSink(&sink);

  // Transitional cutover write (F2-2): with [camera] db configured the legacy
  // resolves the camera-domain reads and writes (camera, camera_stream, zone)
  // to the camera database owned by argus-camera. Cross-process SQLite rules
  // apply on both sides: WAL plus busy_timeout. The legacy never runs DDL on
  // camera.db, so the client installs plain pragmas. Without the key the
  // fallback to the default client keeps the pre-cutover behavior
  // byte-identical.
  const auto path = ConfigService::getString("camera.db");
  if (path.empty())
    return;

  try {
    const auto camera = drogon::orm::DbClient::newSqlite3Client(
        "filename=" + path, 1);
    camera->execSqlSync("PRAGMA journal_mode = WAL");
    camera->execSqlSync("PRAGMA busy_timeout = 5000");
    camera->execSqlSync("PRAGMA synchronous = NORMAL");
    camera->execSqlSync("PRAGMA foreign_keys = ON");
    DbService::setCameraClient(camera);
    LOG_INFO << "Camera database attached read-write: " << path;
  }
  catch (const std::exception& e) {
    LOG_WARN << "Camera database open failed (" << e.what()
             << "); camera reads and writes fall back to the default client";
  }
}

} // namespace

int Application::run()
{
  ConfigService::load("config.toml");

  installIdentityClient();

  app().loadConfigJson(ConfigService::drogonConfig());

  registerIdentitySurface();
  registerCameraSurface();
  installCameraSurface();
  installUserChangeSink();

  installEventBus();

  app().registerPreRoutingAdvice([](const HttpRequestPtr& req,
                                    AdviceCallback&& cb,
                                    AdviceChainCallback&& chain) {
    if (req->method() == Options) {
      AppConfig::handleOptions(req, std::move(cb));
      return;
    }
    chain();
  });

  app().registerPostHandlingAdvice(
      [](const HttpRequestPtr&, const HttpResponsePtr& resp) {
        AppConfig::applyCors(resp);
      });

  app().setExceptionHandler(AppConfig::handleException);

  app().setCustomErrorHandler(
      [](drogon::HttpStatusCode code, const drogon::HttpRequestPtr&) {
        if (code == drogon::k405MethodNotAllowed)
          return AppConfig::get405Response();
        return AppConfig::get404Response();
      });

  app().registerBeginningAdvice([this]() {
    struct sigaction sa{};
    sa.sa_handler = forceShutdownHandler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, nullptr);
    sigaction(SIGTERM, &sa, nullptr);

    struct sigaction crashSa{};
    crashSa.sa_handler = crashHandler;
    sigemptyset(&crashSa.sa_mask);
    crashSa.sa_flags = 0;
    sigaction(SIGSEGV, &crashSa, nullptr);
    sigaction(SIGABRT, &crashSa, nullptr);

    if (!DbService::migrate(5)) {
      LOG_FATAL << "Database migration failed — aborting startup";
      _exit(1);
    }

    DbService::applyPragmas();

    DbService::installExtensions();

    if (!ConfigService::getBool("pairing.paired"))
      printPairingBanner();
  });

  registerServices();

  llama_backend_init();

  if (!registry_.initialize()) {
    LOG_FATAL << "Service initialization failed";
    // Tear services down before freeing the llama backend: skipping this
    // segfaulted in llama_backend_free() with the LLM context still alive.
    registry_.shutdownAll();
    llama_backend_free();
    return 1;
  }

  // At the cutover argus-camera owns the stream lifecycle, so the legacy
  // leaves its go2rtc process down and its ports free. Standalone (no
  // [camera] db) the legacy keeps spawning it, exactly as before.
  if (ConfigService::getString("camera.db").empty()) {
    Go2rtcManager::instance().init();
  }
  else {
    LOG_INFO << "Camera domain delegated; go2rtc stays with argus-camera";
  }
  MediaRelay::instance().init();
  StreamHub::instance().init();

  if (!FaceService::instance().isLoaded()) {
    LOG_WARN << "FaceService not loaded — face recognition disabled. "
             << "Run scripts/setup.sh to download models.";
  }

  LOG_INFO << "Argus backend serving HTTPS on 0.0.0.0:7024";
  app().run();

  shutdown();
  return 0;
}

void Application::shutdown()
{
  StreamHub::instance().shutdown();
  MediaRelay::instance().shutdown();
  Go2rtcManager::instance().shutdown();
  registry_.shutdownAll();
  llama_backend_free();
}

void Application::registerServices()
{
  registry_.registerService(std::make_unique<RoomManagerServiceAdapter>());
  registry_.registerService(std::make_unique<CertServiceAdapter>());
  registry_.registerService(std::make_unique<MdnsServiceAdapter>());
  // TTS cutover (Ruling BI/BJ): with tts.remote_url set the legacy skips the
  // in-process engine entirely — synthesis goes to argus-tts over HTTP and
  // the ONNX models are never loaded here.
  if (ConfigService::getString("tts.remote_url").empty()) {
    registry_.registerService(std::make_unique<TtsServiceAdapter>());
  }
  else {
    LOG_INFO << "TTS delegated to " << ConfigService::getString("tts.remote_url")
             << "; in-process TtsService stays uninitialized";
  }
  registry_.registerService(std::make_unique<LlmServiceAdapter>());
  registry_.registerService(std::make_unique<SttServiceAdapter>());
  registry_.registerService(std::make_unique<VisionServiceAdapter>());
  registry_.registerService(std::make_unique<FaceServiceAdapter>());
  registry_.registerService(std::make_unique<IntentServiceAdapter>());
  registry_.registerService(std::make_unique<MemoryServiceAdapter>());
  registry_.registerService(std::make_unique<QueueManagerServiceAdapter>());
  registry_.registerService(std::make_unique<ExtractionServiceAdapter>());
}
