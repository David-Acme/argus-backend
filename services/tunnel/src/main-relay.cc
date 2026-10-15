#include <drogon/drogon.h>
#include <http/error-handler.hxx>
#include <http/health-controller.hxx>
#include <http/listener-config.hxx>
#include <net/poll-loop.hxx>
#include <server/health-extras.hxx>
#include <server/service-config.hxx>
#include <config/config-service.hxx>

#include <relay/tunnel-relay.hxx>

#include <json/value.h>
#include <memory>
#include <thread>
#include <runtime/log-output.hxx>

int main()
{
  log_output::flushEachLine();
  ConfigService::load("config.toml");

  const RelayConfig config = RelayConfig::resolve();
  if (const auto problem = tunnelSecretProblem(config.relay.secret)) {
    LOG_FATAL << "argus-relay: " << *problem << "; refusing to start";
    return 1;
  }

  PollLoop loop;
  tunnel::TunnelRelay relay(loop, config.relay);

  HealthStatus status = relayHealthStatus(relay);

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

  LOG_INFO << "argus-relay health on " << config.healthHost << ":"
           << config.healthPort;

  drogon::app().setThreadNum(0).run();

  relay.stop();
  loop.stop();
  loopThread.join();
  return 0;
}
