#include <config/app-config.hxx>
#include <csignal>
#include <drogon/drogon.h>
#include <execinfo.h>
#include <memory>
#include <shared/services/app-env-context/app-env-context.hxx>
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
  AppEnvContext::load(".env");

  app().loadConfigFile("./config.json");

  app().registerPostHandlingAdvice([](const HttpRequestPtr&,
                                       const HttpResponsePtr& resp) {
    resp->addHeader("Access-Control-Allow-Origin", "*");
    resp->addHeader("Access-Control-Allow-Methods",
                    "GET, POST, PATCH, PUT, DELETE, OPTIONS");
    resp->addHeader("Access-Control-Allow-Headers",
                    "Content-Type, Authorization, Accept, Accept-Encoding, "
                    "Accept-Language, Origin, Referer, User-Agent, Connection, "
                    "Cache-Control, X-Requested-With");
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

  LOG_INFO << "Argus backend listening on 0.0.0.0:7024";
  app().run();
}
