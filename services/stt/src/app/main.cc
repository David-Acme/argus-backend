#include <app/rpc/stt-rpc-server.hxx>
#include <config/stt-config.hxx>
#include <drogon/drogon.h>
#include <feature/stt/controllers/stt-controller.hxx>
#include <feature/settings/stt-components.hxx>
#include <feature/settings/stt-settings.hxx>
#include <feature/spot/services/keyword-spotter.hxx>
#include <feature/stt/services/stt-service.hxx>
#include <grpc/internal-tls.hxx>
#include <http/error-handler.hxx>
#include <http/health-controller.hxx>
#include <http/listener-config.hxx>
#include <http/tls-health.hxx>
#include <runtime/thread-budget.hxx>
#include <runtime/log-output.hxx>
#include <stt/stt-remote.hxx>
#include <config/config-service.hxx>
#include <settings/settings-rpc.hxx>

#include <json/value.h>
#include <memory>
#include <optional>
#include <string>
#include <unistd.h>
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

}

int main()
{
  log_output::flushEachLine();
  ConfigService::load("config.toml");
  const InternalTlsConfig internalTlsConfig = InternalTlsConfig::resolve();
  std::optional<argus::client::InternalTlsMaterial> internalTls;
  try {
    internalTls = argus::client::installInternalTlsFrom(internalTlsConfig.directory);
  }
  catch (const std::exception& error) {
    LOG_FATAL << error.what() << " — aborting startup";
    _exit(1);
  }
  const auto loadedTls = internalTls.value_or(argus::client::InternalTlsMaterial{});

  const ListenerConfig listener = SttConfig::resolveListener();

  drogon::app().registerController(std::make_shared<HealthController>(HealthStatus{
      .serviceName = "argus-stt",
      .extras = {tls_health::healthExtra({.certificatePem = loadedTls.certificatePem,
                                          .trustBundlePem = loadedTls.trustBundlePem})}}));
  drogon::app().registerController(std::make_shared<SttController>());

  drogon::app().loadConfigJson(drogonConfig(listener));

  drogon::app().setExceptionHandler(ErrorHandler::handleException);

  drogon::app().setCustomErrorHandler(
      [](drogon::HttpStatusCode code, const drogon::HttpRequestPtr&) {
        return ErrorHandler::unmatchedRoute(code);
      });

  SttService::instance().init();
  if (!SttService::instance().isLoaded()) {
    LOG_FATAL << "STT engine failed to load — aborting startup";
    return 1;
  }

  SettingsRegistry settings(sttSettingsCatalog());
  DiskComponentHost components(sttComponents(
      {.modelsDir = SttConfig::resolveComponentsRoot(), .loaded = [] { return SttService::instance().isLoaded(); }}));

  KeywordSpotterService spotter;
  static_cast<void>(spotter.init(SttConfig::resolveSpot()));

  std::unique_ptr<SettingsRpcService> settingsRpc;
  std::unique_ptr<SttRpcServer> rpc;
  const SttRpcConfig rpcConfig = SttConfig::resolveRpc();
  if (!internalTlsConfig.address.empty() && !rpcConfig.credentials.empty()) {
    auto& stt = SttService::instance();
    std::vector<grpc::Service*> services;
    if (!rpcConfig.settingsCredentials.empty()) {
      settingsRpc = std::make_unique<SettingsRpcService>(SettingsRpcInput{
          .service = "stt", .registry = &settings, .credentials = rpcConfig.settingsCredentials});
      settingsRpc->attachComponents(components);
      services.push_back(settingsRpc.get());
    }
    rpc = std::make_unique<SttRpcServer>(SttRpcInput{
        .address = rpcConfig.address,
        .credentials = rpcConfig.credentials,
        .capabilities = [] {
          auto& service = SttService::instance();
          return argus::stt::Capabilities{
              .sampleRate = kWireSampleRate,
              .loaded = service.isLoaded(),
              .language = service.language(),
              .defaultLanguage = SttService::configLanguage(),
              .languages = SttService::supportedLanguages()};
        },
        .acceptsLanguage = [](const std::string& language) {
          return SttService::instance().isSupportedLanguage(language);
        },
        .transcribe = [&stt](const TranscribeRequest& request) {
          return stt.transcribe(request);
        },
        .slots = ThreadBudget::inferenceSlots(),
        .services = std::move(services),
        .tlsAddress = internalTlsConfig.address,
        .tls = internalTls,
        .spot = [&spotter] { return spotter.open(); }});
  }

  if (rpc && !rpcConfig.address.empty())
    LOG_INFO << "argus-stt gRPC transcription listening on "
             << rpcConfig.address;
  if (rpc && rpc->tlsPort() > 0)
    LOG_INFO << "argus-stt gRPC transcription with mutual TLS on " << internalTlsConfig.address;

  LOG_INFO << "argus-stt listening on " << listener.host << ":"
           << listener.port;

  drogon::app()
      .setThreadNum(0)
      .run();

  if (rpc)
    rpc->shutdown();
  SttService::instance().shutdown();
  return 0;
}
