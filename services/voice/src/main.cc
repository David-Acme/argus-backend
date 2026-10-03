#include <config/voice-config.hxx>
#include <feature/health/health-rpc-service.hxx>
#include <feature/voice/voice-rpc-service.hxx>
#include <grpcpp/grpcpp.h>
#include <http/cors.hxx>
#include <http/error-handler.hxx>
#include <http/health-controller.hxx>
#include <http/listener-config.hxx>
#include <config/config-service.hxx>

#include <drogon/drogon.h>

#include <memory>
#include <string>
#include <runtime/log-output.hxx>

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

}

int main()
{
  log_output::flushEachLine();
  ConfigService::load("config.toml");

  const ListenerConfig healthListener = VoiceConfig::resolveHealthListener();
  const GrpcListenerConfig grpcListener = VoiceConfig::resolveGrpcListener();

  VoiceRpcService voiceRpc(VoiceConfig::resolveSyncCallerSecret());
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
        Cors::apply(resp);
      });

  drogon::app().setExceptionHandler(ErrorHandler::handleException);

  drogon::app().setCustomErrorHandler(
      [](drogon::HttpStatusCode code, const drogon::HttpRequestPtr&) {
        return ErrorHandler::unmatchedRoute(code);
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
