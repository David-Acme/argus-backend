#include <app/rpc/tts-rpc-server.hxx>
#include <config/tts-config.hxx>
#include <http/error-handler.hxx>
#include <http/health-controller.hxx>
#include <http/listener-config.hxx>
#include <runtime/thread-budget.hxx>
#include <runtime/log-output.hxx>
#include <feature/synthesis/controllers/tts-controller.hxx>
#include <drogon/drogon.h>
#include <auth/valid-json-filter.hxx>
#include <config/config-service.hxx>
#include <feature/provisioning/services/pocket-provisioning.hxx>
#include <feature/settings/tts-settings.hxx>
#include <feature/synthesis/services/tts-service.hxx>
#include <settings/settings-rpc.hxx>

#include <json/value.h>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

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

  const ListenerConfig listener = TtsConfig::resolveListener();

  drogon::app().registerController(std::make_shared<HealthController>(HealthStatus{.serviceName = "argus-tts", .extras = {}}));
  drogon::app().registerController(std::make_shared<TtsController>());
  drogon::app().registerFilter(std::make_shared<ValidJsonFilter>());

  drogon::app().loadConfigJson(drogonConfig(listener));

  drogon::app().setExceptionHandler(ErrorHandler::handleException);

  drogon::app().setCustomErrorHandler(
      [](drogon::HttpStatusCode code, const drogon::HttpRequestPtr&) {
        return ErrorHandler::unmatchedRoute(code);
      });

  TtsService::instance().init();
  if (!TtsService::instance().isLoaded()) {
    LOG_FATAL << "TTS engine failed to load — aborting startup";
    return 1;
  }

  PocketProvisioning provisioning({.paths = {.modelsDir = TtsService::modelsDirectory(),
                                             .pocketDir = TtsService::pocketModelsDir(),
                                             .toolchainPython = defaultToolchainPython()},
                                   .configFile = std::filesystem::absolute("config.toml")});
  SettingsRegistry settings(ttsSettingsCatalog());
  settings.describeChoices([&provisioning](const SettingSpec& spec) { return provisioning.choiceStates(spec); });
  settings.onChange([&provisioning](const std::vector<std::string>& keys) {
    TtsService::instance().refreshDefaults();
    provisioning.installFor(keys);
  });

  std::unique_ptr<TtsRpcServer> rpc;
  std::unique_ptr<SettingsRpcService> settingsRpc;
  const TtsRpcConfig rpcConfig = TtsConfig::resolveRpc();
  if (!rpcConfig.address.empty() && !rpcConfig.credentials.empty()) {
    auto& synthesis = TtsService::instance();
    std::vector<grpc::Service*> services;
    if (!rpcConfig.settingsCredentials.empty()) {
      settingsRpc = std::make_unique<SettingsRpcService>(SettingsRpcInput{
          .service = "tts", .registry = &settings, .credentials = rpcConfig.settingsCredentials});
      services.push_back(settingsRpc.get());
    }
    rpc = std::make_unique<TtsRpcServer>(TtsRpcInput{
        .address = rpcConfig.address,
        .credentials = rpcConfig.credentials,
        .capabilities = {.sampleRate = synthesis.sampleRate(),
                         .channels = 1,
                         .defaultSpeed = synthesis.defaultSpeed(),
                         .voices = synthesis.availableVoices(),
                         .languages = TtsService::supportedLangs()},
        .synthesize = [&synthesis](TtsStreamInput input) {
          synthesis.synthesizeStream(std::move(input));
        },
        .slots = ThreadBudget::inferenceSlots(),
        .defaultSpeed = [&synthesis] { return synthesis.defaultSpeed(); },
        .services = std::move(services)});
  }

  if (rpc)
    LOG_INFO << "argus-tts gRPC synthesis listening on " << rpcConfig.address;

  LOG_INFO << "argus-tts listening on " << listener.host << ":"
           << listener.port;

  std::jthread warmUp([] { TtsService::instance().warmUp(); });

  drogon::app()
      .setThreadNum(0)
      .run();

  if (rpc)
    rpc->shutdown();
  provisioning.stop();
  TtsService::instance().shutdown();
  return 0;
}
