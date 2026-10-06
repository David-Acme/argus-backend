#include <feature/components/infra/vision-fetch.hxx>
#include <feature/components/services/vision-component-host.hxx>
#include <feature/vlm/services/jpeg-gate.hxx>
#include <app/rpc/vlm-rpc-server.hxx>
#include <config/vlm-config.hxx>
#include <feature/settings/vlm-settings.hxx>
#include <feature/vlm/controllers/vlm-controller.hxx>
#include <drogon/drogon.h>
#include <http/error-handler.hxx>
#include <http/health-controller.hxx>
#include <http/listener-config.hxx>
#include <runtime/thread-budget.hxx>
#include <runtime/log-output.hxx>
#include <vlm/vlm-client.hxx>
#include <config/config-service.hxx>
#include <settings/settings-rpc.hxx>

#include <json/value.h>
#include <llama.h>
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
  limitDecoderPixels();
  log_output::flushEachLine();
  ConfigService::load("config.toml");

  const ListenerConfig listener = VlmConfig::resolveListener();

  drogon::app().registerController(std::make_shared<HealthController>(HealthStatus{.serviceName = "argus-vlm", .extras = {}}));
  const auto vlm = std::make_shared<VlmController>();
  drogon::app().registerController(vlm);

  drogon::app().loadConfigJson(drogonConfig(listener));

  drogon::app().setIdleConnectionTimeout(600);

  drogon::app().setExceptionHandler(ErrorHandler::handleException);

  drogon::app().setCustomErrorHandler(
      [](drogon::HttpStatusCode code, const drogon::HttpRequestPtr&) {
        return ErrorHandler::unmatchedRoute(code);
      });

  llama_backend_init();

  if (resolveVisionModelFiles().present())
    vlm->initEngine();
  if (!vlm->isEngineLoaded())
    LOG_WARN << "argus-vlm: the vision engine is not loaded; it loads once the vision component is installed";

  auto components = std::make_unique<VisionComponentHost>(VisionComponentHostInput{
      .modelsDir = VlmConfig::resolveComponentsRoot(),
      .fetch = visionFetch({.transport = {}, .policy = {}}),
      .loaded = [&vlm] { return vlm->isEngineLoaded(); },
      .load = [&vlm] { vlm->initEngine(); },
      .unload = [&vlm] { vlm->shutdownEngine(); }});

  SettingsRegistry settings(vlmSettingsCatalog());
  settings.onChange([&vlm](const std::vector<std::string>&) { vlm->service().refreshDefaults(); });
  if (llama_supports_gpu_offload())
    settings.declareCapability("gpu");

  std::unique_ptr<SettingsRpcService> settingsRpc;
  std::unique_ptr<VlmRpcServer> rpc;
  const VlmRpcConfig rpcConfig = VlmConfig::resolveRpc();
  if (!rpcConfig.address.empty() && !rpcConfig.credentials.empty()) {
    std::vector<grpc::Service*> services;
    if (!rpcConfig.settingsCredentials.empty()) {
      settingsRpc = std::make_unique<SettingsRpcService>(SettingsRpcInput{
          .service = "vlm", .registry = &settings, .credentials = rpcConfig.settingsCredentials});
      settingsRpc->attachComponents(*components);
      services.push_back(settingsRpc.get());
    }
    rpc = std::make_unique<VlmRpcServer>(VlmRpcInput{
        .address = rpcConfig.address,
        .credentials = rpcConfig.credentials,
        .capabilities = [&vlm] {
          return argus::vlm::Capabilities{
              .loaded = vlm->isEngineLoaded(),
              .maxInputPx = vlm->service().maxInputPx(),
              .defaultMaxTokens = vlm->service().defaultMaxTokens()};
        },
        .describe = [&vlm](const VisionDescribeMatInput& input) {
          return vlm->service().describeMat(input);
        },
        .slots = ThreadBudget::inferenceSlots(),
        .services = std::move(services)});
  }

  if (rpc)
    LOG_INFO << "argus-vlm gRPC vision listening on " << rpcConfig.address;

  LOG_INFO << "argus-vlm listening on " << listener.host << ":"
           << listener.port;

  drogon::app()
      .setThreadNum(0)
      .run();

  if (rpc)
    rpc->shutdown();
  components.reset();
  vlm->shutdownEngine();
  llama_backend_free();
  return 0;
}
