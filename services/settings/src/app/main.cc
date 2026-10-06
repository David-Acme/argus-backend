#include <app/rpc/modules-rpc-service.hxx>
#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/role-filter.hxx>
#include <auth/valid-json-filter.hxx>
#include <config/config-service.hxx>
#include <config/settings-config.hxx>
#include <drogon/drogon.h>
#include <feature/modules/controllers/modules-controller.hxx>
#include <feature/modules/infra/component-owners.hxx>
#include <feature/modules/infra/host-resources.hxx>
#include <feature/modules/infra/module-catalog-file.hxx>
#include <feature/modules/infra/module-event-sink.hxx>
#include <feature/modules/services/module-engine.hxx>
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
#include <grpc/fleet-caller-gate.hxx>
#include <grpc/grpc-server-drain.hxx>
#include <mdns/mdns-service.hxx>
#include <nats/nats-bus.hxx>
#include <nats/nats-subject.hxx>
#include <runtime/log-output.hxx>
#include <runtime/shutdown-signal.hxx>
#include <sqlite/db-service.hxx>

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <memory>
#include <string>
#include <vector>

#include <unistd.h>

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

constexpr std::chrono::milliseconds kGrpcDrainDeadline{2000};

std::int64_t unixMs()
{
  return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::system_clock::now().time_since_epoch())
      .count();
}

drogon::orm::DbClientPtr openModulesDb(const ModulesConfig& config)
{
  auto client = drogon::orm::DbClient::newSqlite3Client("filename=" + config.dbPath, 1);
  if (!client || !DbService::runScriptFile(config.schemaPath, client)) {
    LOG_ERROR << "Modules: " << config.dbPath << " could not apply " << config.schemaPath
              << "; the module routes answer 503";
    return nullptr;
  }
  DbService::applyPragmas(client);
  return client;
}
}

int main()
{
  log_output::flushEachLine();
  ConfigService::load("config.toml");

  try {
    DeviceFilter::requireFingerprintSecret();
  }
  catch (const std::exception& error) {
    LOG_FATAL << error.what() << " — aborting startup";
    _exit(1);
  }

  const ListenerConfig listener = SettingsConfig::resolveListener();
  const auto owners = SettingsConfig::resolveOwners();
  const SettingsGatewayInput gateway{.owners = owners,
                                     .timeouts = SettingsConfig::resolveTimeouts(),
                                     .unconfigured = SettingsConfig::unconfiguredOwners(owners)};
  const auto profiles = loadProfileFile(SettingsConfig::resolveProfilesPath());
  const auto hardware = probeHardwareFacts();
  const auto modules = SettingsConfig::resolveModules();

  NatsBus natsBus;
  if (!natsBus.connect())
    LOG_WARN << "Modules: NATS is not reachable yet; module events are published once it is";
  NatsModuleEventSink moduleEvents(natsBus);
  if (!moduleEvents.ensureStream())
    LOG_WARN << "Modules: the " << nats_subject::kSettingsModuleStream << " stream is not confirmed yet";
  const SettingsComponentOwners componentOwners({.owners = owners, .timeout = gateway.timeouts.update});
  std::unique_ptr<ModuleEngine> moduleEngine;
  if (auto catalog = loadModuleCatalog(modules.catalogPath)) {
    if (auto db = openModulesDb(modules))
      moduleEngine = std::make_unique<ModuleEngine>(ModuleEngineInput{
          .catalog = std::move(*catalog),
          .db = std::move(db),
          .owners = componentOwners,
          .events = &moduleEvents,
          .host =
              [modelsDir = modules.modelsDir] {
                auto host = hostResourcesOf(HardwareProbe::get());
                host.freeDiskBytes = freeDiskBytes(modelsDir);
                return host;
              },
          .clock = unixMs,
          .timing = modules});
  }

  drogon::app().registerController(
      std::make_shared<HealthController>(HealthStatus{.serviceName = "argus-settings", .extras = {}}));
  drogon::app().registerController(std::make_shared<SettingsController>(
      SettingsControllerInput{.gateway = gateway, .profiles = profiles, .hardware = hardware}));
  drogon::app().registerController(std::make_shared<ModulesController>(moduleEngine.get()));

  const auto rpc = SettingsConfig::resolveModulesRpc();
  const auto rpcGate = std::make_shared<const argus::client::FleetCallerGate>(argus::client::FleetGateConfig{
      .expectedCallers = {kModuleStateCallers.begin(), kModuleStateCallers.end()},
      .callerPairs = rpc.callers,
      .legacySecret = {},
      .onFirstLegacy = {}});
  ModulesRpcService modulesRpc(
      [engine = moduleEngine.get()] { return engine != nullptr ? engine->enabledSet() : ModuleStatesReply{}; },
      rpcGate);
  std::unique_ptr<argus::client::GrpcServerDrain> rpcDrain;
  if (!rpc.address.empty()) {
    grpc::ServerBuilder rpcBuilder;
    rpcBuilder.AddListeningPort(rpc.address, grpc::InsecureServerCredentials());
    rpcBuilder.RegisterService(&modulesRpc);
    std::unique_ptr<grpc::Server> rpcServer(rpcBuilder.BuildAndStart());
    if (!rpcServer) {
      LOG_FATAL << "Modules RPC failed to listen on " << rpc.address;
      return 1;
    }
    LOG_INFO << "Modules RPC listening on " << rpc.address << " ("
             << (rpcGate->open() ? std::string("no caller paired")
                                 : std::to_string(rpcGate->pairedCount()) + " paired callers")
             << ")";
    rpcDrain = std::make_unique<argus::client::GrpcServerDrain>(std::move(rpcServer), kGrpcDrainDeadline);
    shutdown_signal::onStop(shutdown_signal::drainOf(*rpcDrain, "settings-modules-rpc"));
  }
  if (moduleEngine)
    shutdown_signal::onStop(shutdown_signal::drainOf(*moduleEngine, "settings-modules"));

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
  drogon::app().registerBeginningAdvice([&moduleEngine]() {
    if (moduleEngine)
      moduleEngine->start();
  });

  drogon::app().setThreadNum(0).run();
  firstRun.requestStop();
  if (moduleEngine)
    moduleEngine->requestStop();
  natsBus.drain();
  return 0;
}
