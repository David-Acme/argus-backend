#include <drogon/drogon.h>
#include <http/error-handler.hxx>
#include <http/health-controller.hxx>
#include <http/listener-config.hxx>
#include <net/poll-loop.hxx>
#include <server/health-extras.hxx>
#include <server/service-config.hxx>
#include <config/config-service.hxx>

#include <client/tunnel-client.hxx>
#include <link/client/link-client.hxx>

#include <json/value.h>
#include <functional>
#include <memory>
#include <thread>
#include <runtime/log-output.hxx>

namespace
{
class LinkLogObserver : public tunnel::link::LinkObserver
{
public:
  void onLinkUp() override
  {
    LOG_INFO << "argus-tunnel: the home link is up";
  }

  void onLinkDown() override
  {
    LOG_INFO << "argus-tunnel: the home link is down";
  }

  void onGoingAway(tunnel::link::GoAwayReason reason,
                   uint16_t seconds) override
  {
    LOG_INFO << "argus-tunnel: the cloud node is draining ("
             << tunnel::link::goAwayReasonToString(reason) << ", " << seconds
             << "s)";
  }

  void onPushIntent(std::string_view intent) override
  {
    LOG_INFO << "argus-tunnel: push intent of " << intent.size() << " bytes";
  }

  void onWireError(tunnel::link::WireError error) override
  {
    LOG_WARN << "argus-tunnel: wire error "
             << tunnel::link::wireErrorToString(error);
  }
};

struct ClientRunner
{
  std::function<void()> start;
  std::function<void()> stop;
};

int serveClient(const ClientConfig& config, const HealthStatus& status,
                const ClientRunner& runner, PollLoop& loop)
{
  std::thread loopThread([&loop] { loop.run(); });

  drogon::app().registerController(std::make_shared<HealthController>(status));

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

  runner.start();
  drogon::app().setThreadNum(0).run();
  runner.stop();
  loop.stop();
  loopThread.join();
  return 0;
}
}

int main()
{
  log_output::flushEachLine();
  ConfigService::load("config.toml");

  const ClientConfig config = ClientConfig::resolve();
  if (const auto problem = linkConfigProblem(config)) {
    LOG_FATAL << "argus-tunnel: " << *problem << "; refusing to start";
    return 1;
  }
  if (!config.linkEnabled) {
    if (const auto problem = tunnelSecretProblem(config.tunnel.secret)) {
      LOG_FATAL << "argus-tunnel: " << *problem << "; refusing to start";
      return 1;
    }
  }

  PollLoop loop;
  tunnel::TunnelClient legacyClient(loop, config.tunnel);
  LinkLogObserver observer;
  std::unique_ptr<tunnel::link::LinkClient> linkClient;
  if (config.linkEnabled) {
    linkClient = std::make_unique<tunnel::link::LinkClient>(
        tunnel::link::LinkClient::Deps{.loop = loop,
                                       .options = config.link,
                                       .observer = &observer});
  }
  const HealthStatus status = linkClient
                                  ? tunnel::linkHealthStatus(*linkClient)
                                         : clientHealthStatus(legacyClient);
  const ClientRunner runner{
      .start = [&] {
        if (linkClient)
          linkClient->start();
        else
          legacyClient.start();
      },
      .stop = [&] {
        if (linkClient)
          linkClient->stop();
        else
          legacyClient.stop();
      }};

  if (config.linkEnabled) {
    LOG_INFO << "argus-tunnel-client health on " << config.healthHost << ":"
             << config.healthPort << ", home link v2 to " << config.link.host
             << ":" << config.link.port << " as home " << config.homeId
             << ", gateway " << config.link.gatewayHost << ":"
             << config.link.gatewayPort;
  } else {
    LOG_INFO << "argus-tunnel-client health on " << config.healthHost << ":"
             << config.healthPort << ", relay " << config.tunnel.relayHost
             << ":" << config.tunnel.relayPort << ", gateway "
             << config.tunnel.gatewayHost << ":" << config.tunnel.gatewayPort;
  }

  return serveClient(config, status, runner, loop);
}
