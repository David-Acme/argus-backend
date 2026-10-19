#include <app/rpc/voice-rpc-server.hxx>
#include <config/voice-config.hxx>
#include <feature/health/health-rpc-service.hxx>
#include <feature/rtc/rtc-agent-service.hxx>
#include <feature/settings/voice-settings.hxx>
#include <feature/voice/voice-rpc-service.hxx>
#include <feature/watch/watch-conversations.hxx>
#include <feature/watch/watch-coordinator.hxx>
#include <grpc/internal-tls.hxx>
#include <grpcpp/grpcpp.h>
#include <http/cors.hxx>
#include <http/error-handler.hxx>
#include <http/health-controller.hxx>
#include <http/listener-config.hxx>
#include <http/tls-health.hxx>
#include <config/config-service.hxx>
#include <livekit/livekit.h>
#include <notification/notification-client.hxx>
#include <settings/settings-rpc.hxx>

#include <drogon/drogon.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include <runtime/log-output.hxx>
#include <runtime/shutdown-signal.hxx>

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
                                                              .spoken = report.openingSpoken,
                                                              .sessionId = {}});
  if (sent != NotificationRpcOutcome::Success)
    LOG_WARN << "Voice: EndCall for " << report.callId << " was not accepted";
}

struct ServerDrainInput
{
  grpc::Server* server{nullptr};
  RtcAgentService* rtc{nullptr};
  const VoiceRpcService* streams{nullptr};
};

class ServerDrain
{
public:
  explicit ServerDrain(ServerDrainInput input) : input_(input) {}

  void requestStop()
  {
    if (stopper_.joinable())
      return;
    stopper_ = std::jthread([this] {
      input_.server->Shutdown(std::chrono::system_clock::now() + kShutdownGrace);
      const auto giveUpAt = std::chrono::steady_clock::now() + kShutdownGrace;
      while (input_.streams != nullptr && !input_.streams->idle() &&
             std::chrono::steady_clock::now() < giveUpAt)
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
      if (input_.rtc != nullptr)
        input_.rtc->shutdown();
      stopped_.store(true);
    });
  }

  [[nodiscard]] bool drained() const { return stopped_.load(); }

  void finish()
  {
    requestStop();
    stopper_.join();
  }

private:
  static constexpr std::chrono::seconds kShutdownGrace{2};

  ServerDrainInput input_;
  std::jthread stopper_;
  std::atomic<bool> stopped_{false};
};

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

  const ListenerConfig healthListener = VoiceConfig::resolveHealthListener();
  const GrpcListenerConfig grpcListener = VoiceConfig::resolveGrpcListener();

  VoiceSessionService sessions;
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
                            .rooms = rtc.get(),
                            .dispatchCleanup = {}});
  const VoiceWatchConfig watchConfig = VoiceConfig::resolveWatch();
  const VoiceCameraConfig cameraConfig = VoiceConfig::resolveCamera();
  std::shared_ptr<const CameraWatchClient> watchCameras;
  if (!cameraConfig.target.empty())
    watchCameras = std::make_shared<CameraWatchClient>(
        CameraWatchClientConfig{.target = cameraConfig.target, .credential = cameraConfig.credential});
  const auto echo = std::make_shared<EchoGuard>(EchoGuardConfig{.tail = watchConfig.echoTail,
                                                                .maxLag = watchConfig.echoMaxLag,
                                                                .history = std::chrono::milliseconds(10000),
                                                                .sampleRate = 16000});
  const VoiceGuardConfig guardConfig = VoiceConfig::resolveGuard();
  std::shared_ptr<const GuardPresenceClient> guardPresence;
  if (!guardConfig.target.empty())
    guardPresence = std::make_shared<GuardPresenceClient>(
        GuardPresenceClientConfig{.target = guardConfig.target, .credential = guardConfig.credential});
  const auto rooms = std::make_shared<RoomDirectory>();
  SttWakeSpotter wakeSpotter;
  std::unique_ptr<WatchCoordinator> watch;
  ConversationManager conversations({.config = watchConfig,
                                     .sessions = &sessions,
                                     .camera = watchCameras,
                                     .echo = echo,
                                     .rooms = rooms,
                                     .tts = &voiceTts(),
                                     .stt = &voiceStt(),
                                     .lang = voiceSystemLang(),
                                     .cameraInfo = [&watch](int64_t cameraId) -> std::optional<WatchCameraInfo> {
                                       return watch ? watch->camera(cameraId) : std::nullopt;
                                     },
                                     .listening = [&watch](int64_t cameraId) {
                                       return watch && watch->listening(cameraId);
                                     },
                                     .resolve = {},
                                     .sameSpeaker = {},
                                     .onUpdate = {},
                                     .onEnded = {}});
  watch = std::make_unique<WatchCoordinator>(WatchCoordinatorInput{
      .config = watchConfig,
      .camera = watchCameras,
      .spotter = &wakeSpotter,
      .stt = &voiceStt(),
      .vad = &voiceVad(),
      .language = voiceLangToString(voiceSystemLang()),
      .echo = echo,
      .guard = guardPresence,
      .rooms = rooms,
      .onWake = [&conversations](const ConfirmedWake& wake) { conversations.onWake(wake); },
      .onFrame = [&conversations](const EarFrame& frame) { conversations.onFrame(frame); },
      .onPlaybackFire = [&conversations](const EarFire& fire) { return conversations.onPlaybackFire(fire); }});
  conversations.start();
  watch->start();

  HealthRpcService healthRpc;
  SettingsRegistry settings(voiceSettingsCatalog());
  std::unique_ptr<SettingsRpcService> settingsRpc;
  if (auto callers = VoiceConfig::resolveSettingsCallers(); !callers.empty())
    settingsRpc = std::make_unique<SettingsRpcService>(SettingsRpcInput{
        .service = "voice", .registry = &settings, .credentials = std::move(callers)});

  std::vector<grpc::Service*> grpcServices{&voiceRpc, &healthRpc};
  if (settingsRpc)
    grpcServices.push_back(settingsRpc.get());
  const VoiceRpcListenInput grpcInput{
      .address = grpcListener.address(),
      .tlsAddress = internalTlsConfig.address,
      .tls = internalTls,
      .services = std::move(grpcServices)};
  auto grpcListening = startVoiceRpc(grpcInput);
  if (const auto failure = voiceRpcFailure(grpcInput, grpcListening)) {
    LOG_ERROR << *failure;
    return 1;
  }
  std::unique_ptr<grpc::Server> server = std::move(grpcListening.server);
  if (grpcListening.tlsPort > 0)
    LOG_INFO << "gRPC with mutual TLS on " << internalTlsConfig.address;

  drogon::app().registerController(std::make_shared<HealthController>(HealthStatus{
      .serviceName = "argus-voice",
      .extras = {tls_health::healthExtra({.certificatePem = loadedTls.certificatePem,
                                          .trustBundlePem = loadedTls.trustBundlePem})}}));

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

  if (grpcListener.port > 0)
    LOG_INFO << "gRPC VoiceService on " << grpcListener.address();
  LOG_INFO << "/health on " << hostPort(healthListener.host,
                                        healthListener.port);

  ServerDrain serverDrain({.server = server.get(), .rtc = rtc.get(), .streams = &voiceRpc});
  shutdown_signal::onStop(shutdown_signal::drainOf(serverDrain, "voice-grpc"));

  drogon::app()
      .setThreadNum(0)
      .run();

  serverDrain.finish();
  watch->shutdown();
  conversations.shutdown();
  if (rtc)
    livekit::shutdown();
  return 0;
}
