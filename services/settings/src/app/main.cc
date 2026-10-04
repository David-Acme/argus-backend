#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/role-filter.hxx>
#include <auth/valid-json-filter.hxx>
#include <config/config-service.hxx>
#include <config/settings-config.hxx>
#include <drogon/drogon.h>
#include <feature/settings/controllers/settings-controller.hxx>
#include <feature/settings/infra/hardware-facts.hxx>
#include <feature/settings/infra/profile-file.hxx>
#include <feature/settings/services/first-run-service.hxx>
#include <http/certificate-reload.hxx>
#include <http/cors.hxx>
#include <http/error-handler.hxx>
#include <http/health-controller.hxx>
#include <http/listener-config.hxx>
#include <http/route-announcements.hxx>
#include <mdns/mdns-service.hxx>
#include <runtime/log-output.hxx>
#include <runtime/shutdown-signal.hxx>

#include <memory>
#include <string>
#include <vector>

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

std::string ownerList(const std::vector<SettingsOwnerConfig>& owners)
{
  std::string names;
  for (const auto& owner : owners) {
    if (!names.empty())
      names += ", ";
    names += owner.name;
  }
  return names.empty() ? "none" : names;
}
}

int main()
{
  log_output::flushEachLine();
  ConfigService::load("config.toml");

  const ListenerConfig listener = SettingsConfig::resolveListener();
  const auto owners = SettingsConfig::resolveOwners();
  const SettingsGatewayInput gateway{.owners = owners,
                                     .timeouts = SettingsConfig::resolveTimeouts(),
                                     .unconfigured = SettingsConfig::unconfiguredOwners(owners)};
  const auto profiles = loadProfileFile(SettingsConfig::resolveProfilesPath());
  const auto hardware = probeHardwareFacts();

  drogon::app().registerController(
      std::make_shared<HealthController>(HealthStatus{.serviceName = "argus-settings", .extras = {}}));
  drogon::app().registerController(std::make_shared<SettingsController>(
      SettingsControllerInput{.gateway = gateway, .profiles = profiles, .hardware = hardware}));

  const SettingsGatewayService firstRunGateway(gateway);
  FirstRunService firstRun(
      {.gateway = firstRunGateway, .catalog = profiles, .hardware = hardware, .config = SettingsConfig::resolveFirstRun()});
  shutdown_signal::onStop(shutdown_signal::drainOf(firstRun, "settings-first-run"));

  drogon::app().registerFilter(std::make_shared<DeviceFilter>());
  drogon::app().registerFilter(std::make_shared<ValidJsonFilter>());
  drogon::app().registerFilter(std::make_shared<JwtFilter>());
  drogon::app().registerFilter(std::make_shared<RoleFilter>());

  drogon::app().loadConfigJson(drogonConfig(listener));
  certificate_reload::watch(listener);

  drogon::app().registerPreRoutingAdvice(
      [](const drogon::HttpRequestPtr& req, drogon::AdviceCallback&& cb, drogon::AdviceChainCallback&& chain) {
        if (req->method() == drogon::Options) {
          Cors::handleOptions(req, std::move(cb));
          return;
        }
        chain();
      });
  drogon::app().registerPostHandlingAdvice(
      [](const drogon::HttpRequestPtr&, const drogon::HttpResponsePtr& resp) { Cors::apply(resp); });

  drogon::app().setExceptionHandler(ErrorHandler::handleException);

  drogon::app().setCustomErrorHandler(
      [](drogon::HttpStatusCode code, const drogon::HttpRequestPtr&) { return ErrorHandler::unmatchedRoute(code); });

  LOG_INFO << "Listening on " << listener.host << ":" << listener.port << (listener.tls ? " (TLS" : " (plain")
           << ", cert " << listener.certPath << "); settings owners: " << ownerList(owners);

  std::unique_ptr<MdnsService> mdnsService;
  drogon::app().registerBeginningAdvice([&mdnsService, &listener]() {
    mdnsService = std::make_unique<MdnsService>(routeAnnouncements({.port = listener.port, .tls = listener.tls}));
    mdnsService->initialize();
  });

  drogon::app().registerBeginningAdvice([&firstRun]() { firstRun.start(); });

  drogon::app().setThreadNum(0).run();
  firstRun.requestStop();
  return 0;
}
