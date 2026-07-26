#include <config/app-config.hxx>
#include <csignal>
#include <drogon/drogon.h>
#include <execinfo.h>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/llm/llm-service.hxx>
#include <shared/services/stt/stt-service.hxx>
#include <shared/services/tts/tts-service.hxx>
#include <shared/services/vision/vision-service.hxx>
#include <unistd.h>

using namespace drogon;

static void crashHandler(int sig)
{
  void* array[32];
  int size = backtrace(array, 32);
  LOG_FATAL << "======= CRASH (signal " << sig << ") BACKTRACE =======";
  backtrace_symbols_fd(array, size, STDERR_FILENO);
  _exit(128 + sig);
}

static void forceShutdownHandler(int)
{
  static int sigCount = 0;
  sigCount++;

  if (sigCount == 1) {
    drogon::app().quit();
    return;
  }

  _exit(128 + SIGINT);
}

int main()
{
  ConfigService::load("config.toml");

  app().loadConfigJson(ConfigService::drogonConfig());

  app().registerPreHandlingAdvice([](const HttpRequestPtr& req,
                                     AdviceCallback&& cb,
                                     AdviceChainCallback&&) {
    if (req->method() == Options) {
      AppConfig::handleOptions(req, std::move(cb));
      return;
    }
    cb(HttpResponsePtr{});
  });

  app().registerPostHandlingAdvice(
      [](const HttpRequestPtr&, const HttpResponsePtr& resp) {
        AppConfig::applyCors(resp);
      });

  app().setExceptionHandler(AppConfig::handleException);

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
  });

  TtsService::init();
  LlmService::init();
  SttService::init();
  VisionService::init();

  if (!TtsService::isLoaded() || !LlmService::isLoaded() ||
      !SttService::isLoaded() || !VisionService::isLoaded()) {
    LOG_FATAL << "Service init failures:"
              << " TTS=" << TtsService::isLoaded()
              << " LLM=" << LlmService::isLoaded()
              << " STT=" << SttService::isLoaded()
              << " Vision=" << VisionService::isLoaded();
    return 1;
  }

  LOG_INFO << "Argus backend listening on 0.0.0.0:7024";
  app().run();

  VisionService::shutdown();
  SttService::shutdown();
  LlmService::shutdown();
  TtsService::shutdown();
}
