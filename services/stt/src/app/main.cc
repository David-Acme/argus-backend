#include <app/rpc/stt-rpc-server.hxx>
#include <config/stt-config.hxx>
#include <drogon/drogon.h>
#include <feature/stt/controllers/stt-controller.hxx>
#include <feature/stt/services/stt-service.hxx>
#include <http/error-handler.hxx>
#include <http/health-controller.hxx>
#include <http/listener-config.hxx>
#include <runtime/thread-budget.hxx>
#include <stt/stt-remote.hxx>
#include <config/config-service.hxx>

#include <json/value.h>
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

}

int main()
{
  ConfigService::load("config.toml");

  const ListenerConfig listener = SttConfig::resolveListener();

  drogon::app().registerController(std::make_shared<HealthController>(HealthStatus{.serviceName = "argus-stt", .extras = {}}));
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

  std::unique_ptr<SttRpcServer> rpc;
  const SttRpcConfig rpcConfig = SttConfig::resolveRpc();
  if (!rpcConfig.address.empty() && !rpcConfig.credentials.empty()) {
    auto& stt = SttService::instance();
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
        .slots = ThreadBudget::inferenceSlots()});
  }

  if (rpc)
    LOG_INFO << "argus-stt gRPC transcription listening on "
             << rpcConfig.address;

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
