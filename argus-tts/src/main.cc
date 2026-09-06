#include <config/app-config.hxx>
#include <controllers/health-controller.hxx>
#include <controllers/tts-controller.hxx>
#include <drogon/drogon.h>
#include <filter/valid-json/valid-json-filter.hxx>
#include <server/listener-config.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/tts/tts-service.hxx>

#include <json/value.h>
#include <cstdlib>
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
  drogon::app().registerController(std::make_shared<TtsController>());
  drogon::app().registerFilter(std::make_shared<ValidJsonFilter>());

  drogon::app().loadConfigJson(drogonConfig(listener));

  drogon::app().setExceptionHandler(AppConfig::handleException);

  drogon::app().setCustomErrorHandler(
      [](drogon::HttpStatusCode code, const drogon::HttpRequestPtr&) {
        if (code == drogon::k405MethodNotAllowed)
          return AppConfig::get405Response();
        return AppConfig::get404Response();
      });

  // The onnxruntime engine is THE capacity of this service (Ruling BG): boot
  // fails loudly rather than serving 503s to the legacy adapters.
  TtsService::instance().init();
  if (!TtsService::instance().isLoaded()) {
    LOG_FATAL << "TTS engine failed to load — aborting startup";
    return 1;
  }

  LOG_INFO << "argus-tts listening on " << listener.host << ":"
           << listener.port;

  drogon::app()
      .setThreadNum(0)
      .run();

  TtsService::instance().shutdown();
  return 0;
}
