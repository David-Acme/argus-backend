#include <drogon/drogon.h>
#include <http/error-handler.hxx>
#include <http/health-controller.hxx>
#include <http/listener-config.hxx>
#include <net/poll-loop.hxx>
#include <server/health-extras.hxx>
#include <server/service-config.hxx>
#include <config/config-service.hxx>

#include <client/tunnel-client.hxx>

#include <json/value.h>
#include <thread>
#include <runtime/log-output.hxx>

int main()
{
  log_output::flushEachLine();
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
  drogon::app().setExceptionHandler(ErrorHandler::handleException);
  drogon::app().setCustomErrorHandler(
      [](drogon::HttpStatusCode code, const drogon::HttpRequestPtr&) {
        return ErrorHandler::unmatchedRoute(code);
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
