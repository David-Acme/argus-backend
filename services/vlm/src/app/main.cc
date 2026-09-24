#include <feature/vlm/controllers/vlm-controller.hxx>
#include <drogon/drogon.h>
#include <http/error-handler.hxx>
#include <http/health-controller.hxx>
#include <http/listener-config.hxx>
#include <config/config-service.hxx>

#include <json/value.h>
#include <llama.h>
#include <string>

namespace
{
Json::Value drogonConfig(const ListenerConfig& listener)
{
  Json::Value config = ConfigService::drogonConfig();
  if (config.isNull())
    config = Json::Value(Json::objectValue);
  config["listeners"] = listenerJson(listener);
  return config;
}

}

int main()
{
  ConfigService::load("config.toml");

  const ListenerConfig listener = ListenerConfig::resolve(7031);

  drogon::app().registerController(std::make_shared<HealthController>(HealthStatus{.serviceName = "argus-vlm", .extras = {}}));
  const auto vlm = std::make_shared<VlmController>();
  drogon::app().registerController(vlm);

  drogon::app().loadConfigJson(drogonConfig(listener));

  drogon::app().setIdleConnectionTimeout(600);

  drogon::app().setExceptionHandler(ErrorHandler::handleException);

  drogon::app().setCustomErrorHandler(
      [](drogon::HttpStatusCode code, const drogon::HttpRequestPtr&) {
        return ErrorHandler::unmatchedRoute(code);
      });

  llama_backend_init();

  vlm->initEngine();
  if (!vlm->isEngineLoaded()) {
    LOG_FATAL << "Vision engine failed to load — aborting startup";
    llama_backend_free();
    return 1;
  }

  LOG_INFO << "argus-vlm listening on " << listener.host << ":"
           << listener.port;

  drogon::app()
      .setThreadNum(0)
      .run();

  vlm->shutdownEngine();
  llama_backend_free();
  return 0;
}
