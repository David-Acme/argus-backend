#include <app/rpc/tts-rpc-server.hxx>
#include <shared/wrapper/thread-budget/thread-budget.hxx>
#include <config/app-config.hxx>
#include <controllers/health-controller.hxx>
#include <feature/synthesis/api/http/controller/tts-controller.hxx>
#include <drogon/drogon.h>
#include <filter/valid-json/valid-json-filter.hxx>
#include <server/listener-config.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <feature/synthesis/domain/tts-service.hxx>

#include <json/value.h>
#include <cstdlib>
#include <string>

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

} // namespace

int main()
{
  ConfigService::load("config.toml");

  const ListenerConfig listener = ListenerConfig::resolve(7029);

  drogon::app().registerController(std::make_shared<HealthController>(HealthStatus{.serviceName = "argus-tts", .extras = {}}));
  drogon::app().registerController(std::make_shared<TtsController>());
  drogon::app().registerFilter(std::make_shared<ValidJsonFilter>());

  drogon::app().loadConfigJson(drogonConfig(listener));

  drogon::app().setExceptionHandler(AppConfig::handleException);

  drogon::app().setCustomErrorHandler(
      [](drogon::HttpStatusCode code, const drogon::HttpRequestPtr&) {
        if (code == drogon::k405MethodNotAllowed)
          return AppConfig::get405Response();
        return AppConfig::get404Response();
      });

  TtsService::instance().init();
  if (!TtsService::instance().isLoaded()) {
    LOG_FATAL << "TTS engine failed to load — aborting startup";
    return 1;
  }

  std::unique_ptr<TtsRpcServer> rpc;
  const auto rpcAddress = ConfigService::getString("rpc.address");
  auto credentials = ConfigService::getStringPairs("rpc.callers");
  std::erase_if(credentials, [](const auto& credential) {
    return credential.first.empty() || credential.second.empty();
  });
  if (!rpcAddress.empty() && !credentials.empty()) {
    auto& synthesis = TtsService::instance();
    rpc = std::make_unique<TtsRpcServer>(TtsRpcInput{
        .address = rpcAddress,
        .credentials = std::move(credentials),
        .capabilities = {.sampleRate = synthesis.sampleRate(),
                         .channels = 1,
                         .defaultSpeed = synthesis.defaultSpeed(),
                         .voices = synthesis.availableVoices(),
                         .languages = TtsService::supportedLangs()},
        .synthesize = [&synthesis](TtsStreamInput input) {
          synthesis.synthesizeStream(std::move(input));
        },
        .slots = ThreadBudget::inferenceSlots()});
  }

  if (rpc)
    LOG_INFO << "argus-tts gRPC synthesis listening on " << rpcAddress;

  LOG_INFO << "argus-tts listening on " << listener.host << ":"
           << listener.port;

  drogon::app()
      .setThreadNum(0)
      .run();

  if (rpc)
    rpc->shutdown();
  TtsService::instance().shutdown();
  return 0;
}
