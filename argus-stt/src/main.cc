#include <config/app-config.hxx>
#include <controllers/health-controller.hxx>
#include <controllers/stt-controller.hxx>
#include <drogon/drogon.h>
#include <server/listener-config.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/stt/stt-service.hxx>

#include <json/value.h>
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
  drogon::app().registerController(std::make_shared<SttController>());

  drogon::app().loadConfigJson(drogonConfig(listener));

  drogon::app().setExceptionHandler(AppConfig::handleException);

  drogon::app().setCustomErrorHandler(
      [](drogon::HttpStatusCode code, const drogon::HttpRequestPtr&) {
        if (code == drogon::k405MethodNotAllowed)
          return AppConfig::get405Response();
        return AppConfig::get404Response();
      });

  SttService::instance().init();
  if (!SttService::instance().isLoaded()) {
    LOG_FATAL << "STT engine failed to load — aborting startup";
    return 1;
  }

  LOG_INFO << "argus-stt listening on " << listener.host << ":"
           << listener.port;

  drogon::app()
      .setThreadNum(0)
      .run();

  SttService::instance().shutdown();
  return 0;
}
