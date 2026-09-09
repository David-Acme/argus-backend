#include <config/app-config.hxx>
#include <controllers/health-controller.hxx>
#include <drogon/drogon.h>
#include <net/poll-loop.hxx>
#include <server/health-extras.hxx>
#include <server/listener-config.hxx>
#include <server/service-config.hxx>
#include <shared/services/config-service/config-service.hxx>

#include <client/tunnel-client.hxx>

#include <json/value.h>
#include <thread>

int main()
{
  ConfigService::load("config.toml");

  const ClientConfig config = ClientConfig::resolve();
  if (config.tunnel.secret.empty()) {
    LOG_FATAL << "argus-tunnel: [tunnel] secret is empty; refusing to start";
    return 1;
  }

  PollLoop loop;
  tunnel::TunnelClient client(loop, config.tunnel);

  HealthStatus status = clientHealthStatus(client);

  std::thread loopThread([&loop] { loop.run(); });

  drogon::app().registerController(
      std::make_shared<HealthController>(status));

  Json::Value drogonConfig = ConfigService::drogonConfig();
  if (drogonConfig.isNull())
    drogonConfig = Json::Value(Json::objectValue);
  drogonConfig["listeners"] = listenerJson(
      ListenerConfig{.host = config.healthHost,
                     .port = config.healthPort,
                     .tls = false,
                     .certPath = {},
                     .keyPath = {},
                     .minTlsProtocol = {}});

  drogon::app().loadConfigJson(drogonConfig);
  drogon::app().setExceptionHandler(AppConfig::handleException);
  drogon::app().setCustomErrorHandler(
      [](drogon::HttpStatusCode code, const drogon::HttpRequestPtr&) {
        if (code == drogon::k405MethodNotAllowed)
          return AppConfig::get405Response();
        return AppConfig::get404Response();
      });

  LOG_INFO << "argus-tunnel-client health on " << config.healthHost << ":"
           << config.healthPort << ", relay " << config.tunnel.relayHost
           << ":" << config.tunnel.relayPort << ", gateway "
           << config.tunnel.gatewayHost << ":" << config.tunnel.gatewayPort;

  client.start();

  drogon::app().setThreadNum(0).run();

  client.stop();
  loop.stop();
  loopThread.join();
  return 0;
}
