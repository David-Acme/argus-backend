#include <chrono>
#include <config/app-config.hxx>
#include <csignal>
#include <cstring>
#include <drogon/drogon.h>
#include <execinfo.h>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/llm/llm-service.hxx>
#include <shared/services/stt/stt-service.hxx>
#include <shared/services/tts/tts-service.hxx>
#include <shared/services/vision/vision-service.hxx>
#include <test/conversation-test.hxx>
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

int main(int argc, char* argv[])
{
  if (argc > 1 && std::strcmp(argv[1], "--test-conversation") == 0) {
    ConfigService::load("config.toml");
    argus::test::runConversationTest();
    return 0;
  }

  if (argc > 1 && std::strcmp(argv[1], "--test-llm-quick") == 0) {
    ConfigService::load("config.toml");
    LlmService::init();
    if (!LlmService::isLoaded()) {
      LOG_FATAL << "LLM failed to load";
      return 1;
    }
    ChatRequest req;
    req.messages = {
        {"system", "Eres Argus, un sistema de seguridad inteligente. Responde en español."},
        {"user", "Hola, ¿cómo estás?"}
    };
    req.maxTokens = 64;
    req.temperature = 0.7f;
    LOG_INFO << "[LLM-QUICK] Sending test chat (short prompt)...";
    auto start = std::chrono::high_resolution_clock::now();
    auto resp = LlmService::chat(req);
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::high_resolution_clock::now() - start).count();
    LOG_INFO << "[LLM-QUICK] Response: " << resp;
    LOG_INFO << "[LLM-QUICK] Total: " << elapsed << "ms, ~" << resp.size() << " chars";

    LlmService::shutdown();
    LOG_INFO << "[LLM-QUICK] Done";
    return 0;
  }

  if (argc > 1 && std::strcmp(argv[1], "--bench-tts") == 0) {
    ConfigService::load("config.toml");
    TtsService::init();
    if (!TtsService::isLoaded()) {
      LOG_FATAL << "TTS failed to load";
      return 1;
    }

    const std::string text =
        "¡Hola! Estoy funcionando correctamente, gracias por preguntar. "
        "¿En qué puedo ayudarte con seguridad en tu hogar?";

    for (int steps : {5, 8, 10}) {
      TtsRequest req;
      req.text = text;
      req.lang = TtsLang::ES;
      req.voiceId = "M3";
      req.speed = 1.05f;
      req.quality = steps == 5   ? TtsQuality::Low
                     : steps == 8 ? TtsQuality::Medium
                                  : TtsQuality::High;

      auto start = std::chrono::high_resolution_clock::now();
      auto audio = TtsService::synthesize(req);
      auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
          std::chrono::high_resolution_clock::now() - start).count();
      float audioSec = static_cast<float>(audio.size()) / TtsService::sampleRate();
      float rtf = elapsed / 1000.0f / audioSec;
      LOG_INFO << "[TTS] steps=" << steps << " | " << elapsed << "ms | audio="
               << audioSec << "s | RTF=" << rtf << "x";
    }

    TtsService::shutdown();
    return 0;
  }

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
