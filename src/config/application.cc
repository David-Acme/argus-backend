#include "application.hxx"

#include <cctype>
#include <config/app-config.hxx>
#include <csignal>
#include <drogon/drogon.h>
#include <execinfo.h>
#include <iostream>
#include <llama.h>
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
#include <shared/services/sqlite/db-service.hxx>
#include <shared/services/stream/go2rtc-manager.hxx>
#include <shared/services/stream/media-relay.hxx>
#include <shared/services/stream/stream-hub.hxx>
#include <shared/services/stt/adapter/stt-service-adapter.hxx>
#include <shared/services/tts/adapter/tts-service-adapter.hxx>
#include <shared/services/vision/adapter/vision-service-adapter.hxx>
#include <shared/wrapper/qr/qr-render.hxx>
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
            << qr_render::asciiQr(payload.toStyledString()) << "\n"
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

} // namespace

int Application::run()
{
  ConfigService::load("config.toml");

  app().loadConfigJson(ConfigService::drogonConfig());

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

    if (!DbService::migrate(1)) {
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
    llama_backend_free();
    return 1;
  }

  Go2rtcManager::instance().init();
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
  registry_.registerService(std::make_unique<TtsServiceAdapter>());
  registry_.registerService(std::make_unique<LlmServiceAdapter>());
  registry_.registerService(std::make_unique<SttServiceAdapter>());
  registry_.registerService(std::make_unique<VisionServiceAdapter>());
  registry_.registerService(std::make_unique<FaceServiceAdapter>());
  registry_.registerService(std::make_unique<IntentServiceAdapter>());
  registry_.registerService(std::make_unique<MemoryServiceAdapter>());
  registry_.registerService(std::make_unique<QueueManagerServiceAdapter>());
  registry_.registerService(std::make_unique<ExtractionServiceAdapter>());
}
