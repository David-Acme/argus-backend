#include <config/app-config.hxx>
#include <controllers/health-controller.hxx>
#include <controllers/llm-controller.hxx>
#include <drogon/drogon.h>
#include <server/listener-config.hxx>
#include <shared/services/config-service/config-service.hxx>

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

} // namespace

int main()
{
  ConfigService::load("config.toml");

  const ListenerConfig listener = ListenerConfig::resolve();

  drogon::app().registerController(std::make_shared<HealthController>());
  const auto llm = std::make_shared<LlmController>();
  drogon::app().registerController(llm);

  drogon::app().loadConfigJson(drogonConfig(listener));

  drogon::app().setExceptionHandler(AppConfig::handleException);

  drogon::app().setCustomErrorHandler(
      [](drogon::HttpStatusCode code, const drogon::HttpRequestPtr&) {
        if (code == drogon::k405MethodNotAllowed)
          return AppConfig::get405Response();
        return AppConfig::get404Response();
      });

  llama_backend_init();

  // The chat engine is THE capacity of this service (Ruling BS): boot fails
  // loudly rather than serving 503s to the voice session.
  llm->initEngine();
  if (!llm->isEngineLoaded()) {
    LOG_FATAL << "LLM engine failed to load — aborting startup";
    llama_backend_free();
    return 1;
  }

  LOG_INFO << "argus-llm listening on " << listener.host << ":"
           << listener.port;

  drogon::app()
      .setThreadNum(0)
      .run();

  llm->shutdownEngine();
  llama_backend_free();
  return 0;
}
