#include "application.hxx"

#include <config/app-config.hxx>
#include <csignal>
#include <drogon/drogon.h>
#include <execinfo.h>
#include <llama.h>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/face/adapter/face-service-adapter.hxx>
#include <shared/services/face/face-db.hxx>
#include <shared/services/face/face-service.hxx>
#include <shared/services/llm/adapter/llm-service-adapter.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/services/stt/adapter/stt-service-adapter.hxx>
#include <shared/services/tts/adapter/tts-service-adapter.hxx>
#include <shared/services/vision/adapter/vision-service-adapter.hxx>
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

  app().setCustomErrorHandler([](drogon::HttpStatusCode code,
                                 const drogon::HttpRequestPtr&) {
    if (code == drogon::k405MethodNotAllowed)
      return AppConfig::get405Response();
    return AppConfig::get404Response();
  });

  app().registerBeginningAdvice([]() {
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

    FaceDB::loadFromDb();
  });

  registerServices();

  llama_backend_init();

  if (!registry_.initialize()) {
    LOG_FATAL << "Service initialization failed";
    llama_backend_free();
    return 1;
  }

  if (!FaceService::isLoaded()) {
    LOG_WARN << "FaceService not loaded — face recognition disabled. "
             << "Run scripts/setup.sh to download models.";
  }

  LOG_INFO << "Argus backend listening on 0.0.0.0:7024";
  app().run();

  shutdown();
  return 0;
}

void Application::shutdown()
{
  registry_.shutdownAll();
  llama_backend_free();
}

void Application::registerServices()
{
  registry_.registerService(std::make_unique<TtsServiceAdapter>());
  registry_.registerService(std::make_unique<LlmServiceAdapter>());
  registry_.registerService(std::make_unique<SttServiceAdapter>());
  registry_.registerService(std::make_unique<VisionServiceAdapter>());
  registry_.registerService(std::make_unique<FaceServiceAdapter>());
}
