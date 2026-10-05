#include <config/voice-config.hxx>
#include <feature/health/health-rpc-service.hxx>
#include <feature/rtc/rtc-agent-service.hxx>
#include <feature/settings/voice-settings.hxx>
#include <feature/voice/voice-rpc-service.hxx>
#include <grpcpp/grpcpp.h>
#include <http/cors.hxx>
#include <http/error-handler.hxx>
#include <http/health-controller.hxx>
#include <http/listener-config.hxx>
#include <config/config-service.hxx>
#include <livekit/livekit.h>
#include <notification/notification-client.hxx>
#include <settings/settings-rpc.hxx>

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

argus::notification::v1::CallOutcome callOutcomeToProto(rtc_wire::CallOutcome outcome)
{
  switch (outcome) {
    case rtc_wire::CallOutcome::Completed:
      return argus::notification::v1::CALL_OUTCOME_COMPLETED;
    case rtc_wire::CallOutcome::Declined:
      return argus::notification::v1::CALL_OUTCOME_DECLINED;
    case rtc_wire::CallOutcome::Failed:
    case rtc_wire::CallOutcome::NotReported:
      break;
  }
  return argus::notification::v1::CALL_OUTCOME_FAILED;
}

void reportCallEnd(const NotificationClient* notifications, const RtcCallReport& report)
{
  const rtc_wire::CallOutcome outcome = rtc_wire::callOutcomeOf({.callId = report.callId,
                                                                 .reason = report.reason,
                                                                 .userJoined = report.userJoined,
                                                                 .openingSpoken = report.openingSpoken});
  if (outcome == rtc_wire::CallOutcome::NotReported)
    return;
  if (notifications == nullptr) {
    LOG_WARN << "Voice: call " << report.callId << " ended but no notification target is configured";
    return;
  }
  const NotificationRpcOutcome sent = notifications->endCall({.callId = report.callId,
                                                              .userId = report.userId,
                                                              .outcome = callOutcomeToProto(outcome),
                                                              .spoken = report.openingSpoken});
  if (sent != NotificationRpcOutcome::Success)
    LOG_WARN << "Voice: EndCall for " << report.callId << " was not accepted";
}

}

int main()
{
  log_output::flushEachLine();
  ConfigService::load("config.toml");

  const ListenerConfig healthListener = VoiceConfig::resolveHealthListener();
  const GrpcListenerConfig grpcListener = VoiceConfig::resolveGrpcListener();

  VoiceSessionService sessions;
  sessions.warmFarewells();
  const VoiceNotificationConfig notificationConfig = VoiceConfig::resolveNotification();
  std::unique_ptr<NotificationClient> notifications;
  if (!notificationConfig.target.empty())
    notifications = std::make_unique<NotificationClient>(NotificationClientConfig{
        .target = notificationConfig.target, .credential = notificationConfig.credential});

  const VoiceRtcConfig rtcConfig = VoiceConfig::resolveRtc();
  std::unique_ptr<RtcAgentService> rtc;
  if (rtcConfig.enabled) {
    livekit::initialize(livekit::LogLevel::Warn);
    rtc = std::make_unique<RtcAgentService>(RtcAgentInput{
        .sessions = &sessions,
        .config = {.url = rtcConfig.url,
                   .timings = {.rejoinGrace = rtcConfig.rejoinGrace,
                               .firstJoinWait = rtcConfig.firstJoinWait,
                               .thinkingTimeout = RtcCallTimings{}.thinkingTimeout,
                               .connectTimeout = RtcCallTimings{}.connectTimeout}},
        .onCallEnded = [client = notifications.get()](const RtcCallReport& report) {
          reportCallEnd(client, report);
        }});
    LOG_INFO << "Voice: realtime calls join LiveKit at " << rtcConfig.url;
  }

  VoiceRpcService voiceRpc({.sessions = &sessions,
                            .syncCallerSecret = VoiceConfig::resolveSyncCallerSecret(),
                            .notificationCallerSecret = VoiceConfig::resolveNotificationCallerSecret(),
                            .rooms = rtc.get()});
  HealthRpcService healthRpc;
  SettingsRegistry settings(voiceSettingsCatalog());
  std::unique_ptr<SettingsRpcService> settingsRpc;
  if (auto callers = VoiceConfig::resolveSettingsCallers(); !callers.empty())
    settingsRpc = std::make_unique<SettingsRpcService>(SettingsRpcInput{
        .service = "voice", .registry = &settings, .credentials = std::move(callers)});

  grpc::ServerBuilder builder;
  builder.AddListeningPort(hostPort(grpcListener.host, grpcListener.port),
                           grpc::InsecureServerCredentials());
  builder.RegisterService(&voiceRpc);
  builder.RegisterService(&healthRpc);
  if (settingsRpc)
    builder.RegisterService(settingsRpc.get());
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
  if (rtc) {
    rtc->shutdown();
    livekit::shutdown();
  }
  return 0;
}
