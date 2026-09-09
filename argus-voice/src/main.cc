#include <config/app-config.hxx>
#include <controllers/health-controller.hxx>
#include <feature/health/health-rpc-service.hxx>
#include <feature/voice/voice-rpc-service.hxx>
#include <grpcpp/grpcpp.h>
#include <server/listener-config.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/wrapper/api-response/api-response.hxx>

#include <drogon/drogon.h>

#include <memory>
#include <string>

namespace
{

std::string hostPort(const std::string& host, uint16_t port)
{
  return host + ":" + std::to_string(port);
}

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

  const ListenerConfig healthListener = ListenerConfig::resolve(7035, "server.health_port");
  const GrpcListenerConfig grpcListener = GrpcListenerConfig::resolve(7034);

  VoiceRpcService voiceRpc;
  HealthRpcService healthRpc;

  grpc::ServerBuilder builder;
  builder.AddListeningPort(hostPort(grpcListener.host, grpcListener.port),
                           grpc::InsecureServerCredentials());
  builder.RegisterService(&voiceRpc);
  builder.RegisterService(&healthRpc);
  std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  if (!server) {
    LOG_ERROR << "gRPC server failed to listen on "
              << hostPort(grpcListener.host, grpcListener.port);
    return 1;
  }

  drogon::app().registerController(std::make_shared<HealthController>(HealthStatus{.serviceName = "argus-voice", .extras = {}}));

  drogon::app().loadConfigJson(drogonConfig(healthListener));

  drogon::app().registerPostHandlingAdvice(
      [](const drogon::HttpRequestPtr&, const drogon::HttpResponsePtr& resp) {
        AppConfig::applyCors(resp);
      });

  drogon::app().setExceptionHandler(AppConfig::handleException);

  drogon::app().setCustomErrorHandler(
      [](drogon::HttpStatusCode code, const drogon::HttpRequestPtr&) {
        if (code == drogon::k405MethodNotAllowed)
          return AppConfig::get405Response();
        return AppConfig::get404Response();
      });

  LOG_INFO << "gRPC VoiceService on " << hostPort(grpcListener.host,
                                                  grpcListener.port)
           << "; /health on " << hostPort(healthListener.host,
                                          healthListener.port);

  drogon::app()
      .setThreadNum(0)
      .run();

  server->Shutdown();
  return 0;
}
