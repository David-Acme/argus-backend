#include <config/app-config.hxx>
#include <controllers/health-controller.hxx>
#include <drogon/drogon.h>
#include <net/poll-loop.hxx>
#include <server/service-config.hxx>
#include <shared/services/config-service/config-service.hxx>

#include <relay/tunnel-relay.hxx>

#include <json/value.h>
#include <thread>

int main()
{
  ConfigService::load("config.toml");

  const RelayConfig config = RelayConfig::resolve();
  if (config.relay.secret.empty()) {
    LOG_FATAL << "argus-relay: [tunnel] secret is empty; refusing to start";
    return 1;
  }

  PollLoop loop;
  tunnel::TunnelRelay relay(loop, config.relay);

  HealthStatus status;
  status.serviceName = "argus-relay";
  status.homeConnected = [&relay] { return relay.hasHome(); };
  status.activeStreams = [&relay] {
    return static_cast<int>(relay.streamCount());
  };

  std::thread loopThread([&loop] { loop.run(); });

  if (!relay.start()) {
    LOG_FATAL << "argus-relay: could not bind device/home listeners";
    drogon::app().setThreadNum(0);
    loop.stop();
    loopThread.join();
    return 1;
  }

  drogon::app().registerController(
      std::make_shared<HealthController>(status));

  Json::Value drogonConfig = ConfigService::drogonConfig();
  if (drogonConfig.isNull())
    drogonConfig = Json::Value(Json::objectValue);
  drogonConfig["listeners"] =
      healthListenerJson(config.healthHost, config.healthPort);

  drogon::app().loadConfigJson(drogonConfig);
  drogon::app().setExceptionHandler(AppConfig::handleException);
  drogon::app().setCustomErrorHandler(
      [](drogon::HttpStatusCode code, const drogon::HttpRequestPtr&) {
        if (code == drogon::k405MethodNotAllowed)
          return AppConfig::get405Response();
        return AppConfig::get404Response();
      });

  LOG_INFO << "argus-relay health on " << config.healthHost << ":"
           << config.healthPort;

  drogon::app().setThreadNum(0).run();

  relay.stop();
  loop.stop();
  loopThread.join();
  return 0;
}
