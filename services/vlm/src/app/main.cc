#include <app/rpc/vlm-rpc-server.hxx>
#include <feature/vlm/controllers/vlm-controller.hxx>
#include <drogon/drogon.h>
#include <http/error-handler.hxx>
#include <http/health-controller.hxx>
#include <http/listener-config.hxx>
#include <runtime/thread-budget.hxx>
#include <vlm/vlm-client.hxx>
#include <config/config-service.hxx>

#include <json/value.h>
#include <llama.h>
#include <memory>
#include <string>
#include <utility>
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

  const ListenerConfig listener = ListenerConfig::resolve(7031);

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

  vlm->initEngine();
  if (!vlm->isEngineLoaded()) {
    LOG_FATAL << "Vision engine failed to load — aborting startup";
    llama_backend_free();
    return 1;
  }

  std::unique_ptr<VlmRpcServer> rpc;
  const auto rpcAddress = ConfigService::getString("rpc.address");
  auto credentials = ConfigService::getStringPairs("rpc.callers");
  std::erase_if(credentials, [](const auto& credential) {
    return credential.first.empty() || credential.second.empty();
  });
  if (!rpcAddress.empty() && !credentials.empty()) {
    rpc = std::make_unique<VlmRpcServer>(VlmRpcInput{
        .address = rpcAddress,
        .credentials = std::move(credentials),
        .capabilities = [&vlm] {
          return argus::vlm::Capabilities{
              .loaded = vlm->isEngineLoaded(),
              .maxInputPx = vlm->service().maxInputPx(),
              .defaultMaxTokens = vlm->service().defaultMaxTokens()};
        },
        .describe = [&vlm](const VisionDescribeMatInput& input) {
          return vlm->service().describeMat(input);
        },
        .slots = ThreadBudget::inferenceSlots()});
  }

  if (rpc)
    LOG_INFO << "argus-vlm gRPC vision listening on " << rpcAddress;

  LOG_INFO << "argus-vlm listening on " << listener.host << ":"
           << listener.port;

  drogon::app()
      .setThreadNum(0)
      .run();

  if (rpc)
    rpc->shutdown();
  vlm->shutdownEngine();
  llama_backend_free();
  return 0;
}
