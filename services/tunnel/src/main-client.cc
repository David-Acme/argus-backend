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
#include <link/push/sealed-push-bridge.hxx>
#include <link/status/link-status.hxx>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>

#include <json/value.h>
#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <utility>
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

  void onPushResult(const PushResultEnvelope& result) override
  {
    if (sealed != nullptr)
      sealed->onPushResult(result);
  }

  void onWireError(tunnel::link::WireError error) override
  {
    LOG_WARN << "argus-tunnel: wire error "
             << tunnel::link::wireErrorToString(error);
  }

  tunnel::link::SealedPushBridge* sealed{nullptr};
};

struct SealedPushWiring
{
  std::shared_ptr<NatsBus> bus;
  std::unique_ptr<tunnel::link::SealedPushBridge> bridge;
};

constexpr double kSealedPushTickSeconds = 1.0;

SealedPushWiring sealedPushWiring(PollLoop& loop)
{
  const std::string natsUrl = ConfigService::getString("nats.url");
  if (natsUrl.empty()) {
    LOG_INFO << "argus-tunnel: no nats.url, so sealed pushes stay off";
    return {};
  }
  auto bus = std::make_shared<NatsBus>();
  if (!bus->connect())
    LOG_WARN << "argus-tunnel: NATS unavailable at " << natsUrl
             << "; the sealed push consumer attaches once it connects";
  auto bridge = std::make_unique<tunnel::link::SealedPushBridge>(loop, bus);
  return {.bus = std::move(bus), .bridge = std::move(bridge)};
}

struct ClientRunner
{
  std::function<void()> start;
  std::function<void()> stop;
};

constexpr std::chrono::seconds kLinkStateRepublish{30};
constexpr double kLinkStateTickSeconds = 1.0;

int64_t epochSeconds()
{
  return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

struct LinkStateWiring
{
  std::shared_ptr<NatsBus> bus;
  std::unique_ptr<tunnel::link::LinkStatusPublisher> publisher;
  std::optional<trantor::TimerId> timer;
};

LinkStateWiring linkStateWiring(const ClientConfig& config, const tunnel::link::LinkClient& client)
{
  const std::string natsUrl = ConfigService::getString("nats.url");
  if (natsUrl.empty()) {
    LOG_INFO << "argus-tunnel: no nats.url, so the link state stays off the home's bus";
    return {};
  }
  const auto domain = tunnel::link::deviceDomainOf(config.link.deviceDomainSuffix);
  if (!domain) {
    LOG_WARN << "argus-tunnel: the device domain " << config.link.deviceDomainSuffix
             << " is not a DNS name, so the link state is not published";
    return {};
  }
  auto bus = std::make_shared<NatsBus>();
  if (!bus->connect())
    LOG_WARN << "argus-tunnel: NATS unavailable at " << natsUrl
             << "; the link state is published once it connects";
  const std::weak_ptr<NatsBus> weakBus = bus;
  auto publisher = std::make_unique<tunnel::link::LinkStatusPublisher>(
      tunnel::link::LinkStatusPublisherDeps{
          .sample =
              [&client] {
                return tunnel::link::LinkSample{.connected = client.connected(),
                                                .attempts = client.reconnectAttempts()};
              },
          .publish =
              [weakBus](std::string_view payload) {
                const auto locked = weakBus.lock();
                return locked && locked->publish(nats_subject::kTunnelLink, payload);
              },
          .now = epochSeconds},
      tunnel::link::LinkStatusPublisherOptions{
          .homeId = config.homeId, .domain = *domain, .republish = kLinkStateRepublish});
  LOG_INFO << "argus-tunnel: the link state goes to " << nats_subject::kTunnelLink << " as home "
           << config.homeId << " under " << *domain;
  return {.bus = std::move(bus), .publisher = std::move(publisher), .timer = std::nullopt};
}

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
  SealedPushWiring sealed = config.linkEnabled ? sealedPushWiring(loop) : SealedPushWiring{};
  observer.sealed = sealed.bridge.get();
  tunnel::link::LinkOptions linkOptions = config.link;
  if (sealed.bridge)
    linkOptions.capabilities |= tunnel::link::capability::kSealedPush;
  std::unique_ptr<tunnel::link::LinkClient> linkClient;
  if (config.linkEnabled) {
    linkClient = std::make_unique<tunnel::link::LinkClient>(
        tunnel::link::LinkClient::Deps{.loop = loop,
                                       .options = linkOptions,
                                       .observer = &observer});
    if (sealed.bridge)
      sealed.bridge->attach(*linkClient);
  }
  const HealthStatus status = linkClient
                                  ? tunnel::linkHealthStatus(*linkClient)
                                         : clientHealthStatus(legacyClient);
  LinkStateWiring linkState = linkClient ? linkStateWiring(config, *linkClient) : LinkStateWiring{};
  std::optional<trantor::TimerId> sealedTimer;
  const ClientRunner runner{
      .start = [&] {
        if (linkClient)
          linkClient->start();
        else
          legacyClient.start();
        if (linkState.publisher) {
          tunnel::link::LinkStatusPublisher* publisher = linkState.publisher.get();
          publisher->tick();
          linkState.timer = drogon::app().getLoop()->runEvery(kLinkStateTickSeconds,
                                                              [publisher] { publisher->tick(); });
        }
        if (sealed.bridge) {
          tunnel::link::SealedPushBridge* bridge = sealed.bridge.get();
          bridge->tick();
          sealedTimer = drogon::app().getLoop()->runEvery(kSealedPushTickSeconds,
                                                          [bridge] { bridge->tick(); });
        }
      },
      .stop = [&] {
        if (linkState.timer)
          drogon::app().getLoop()->invalidateTimer(*linkState.timer);
        linkState.timer.reset();
        if (sealedTimer)
          drogon::app().getLoop()->invalidateTimer(*sealedTimer);
        sealedTimer.reset();
        if (sealed.bridge)
          sealed.bridge->unsubscribe();
        if (linkClient)
          linkClient->stop();
        else
          legacyClient.stop();
        if (linkState.bus)
          linkState.bus->drain();
        if (sealed.bus)
          sealed.bus->drain();
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
