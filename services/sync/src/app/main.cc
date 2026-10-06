#include <app/rpc/sync-control-callers.hxx>
#include <app/rpc/sync-control-rpc-service.hxx>
#include <config/config-service.hxx>
#include <config/sync-config.hxx>
#include <drogon/drogon.h>
#include <feature/fanout/services/audit-fan-out.hxx>
#include <feature/fanout/services/audit-retention-service.hxx>
#include <feature/fanout/services/change-feed-consumer.hxx>
#include <feature/fanout/services/notification-delivery-consumer.hxx>
#include <feature/fanout/services/sync-fan-out.hxx>
#include <feature/activity/controllers/activity-controller.hxx>
#include <feature/heartbeat/controllers/heartbeat-controller.hxx>
#include <feature/module-data/services/sync-module-data.hxx>
#include <feature/heartbeat/infra/guard-presence-directory.hxx>
#include <feature/heartbeat/services/heartbeat-feed.hxx>
#include <feature/heartbeat/services/heartbeat-service.hxx>
#include <feature/heartbeat/services/presence-board.hxx>
#include <feature/rtc/controllers/rtc-controller.hxx>
#include <feature/rtc/infra/livekit-room-client.hxx>
#include <feature/rtc/infra/notification-call-claimer.hxx>
#include <feature/rtc/infra/voice-room-joiner.hxx>
#include <feature/rtc/services/rtc-session-revoker.hxx>
#include <feature/transport/infra/cached-user-directory.hxx>
#include <feature/transport/infra/camera-sync-gateway.hxx>
#include <feature/transport/infra/identity-sync-gateway.hxx>
#include <feature/transport/infra/notification-sync-gateway.hxx>
#include <feature/transport/infra/productivity-sync-gateway.hxx>
#include <feature/transport/infra/sync-socket-registrar.hxx>
#include <feature/transport/infra/voice-grpc-relay.hxx>
#include <feature/transport/services/connection-lanes.hxx>
#include <auth/device-filter.hxx>
#include <auth/user-directory-identity.hxx>
#include <chrono>
#include <grpc/fleet-caller-gate.hxx>
#include <grpc/grpc-server-drain.hxx>
#include <grpcpp/grpcpp.h>
#include <http/cors.hxx>
#include <http/error-handler.hxx>
#include <http/health-controller.hxx>
#include <http/certificate-reload.hxx>
#include <http/listener-config.hxx>
#include <http/route-announcements.hxx>
#include <json/value.h>
#include <mdns/mdns-service.hxx>
#include <memory>
#include <nats/nats-bus.hxx>
#include <nats/nats-push-intent-sink.hxx>
#include <nats/nats-subject.hxx>
#include <notification/notification-client.hxx>
#include <voice/voice-client.hxx>
#include <runtime/shutdown-signal.hxx>
#include <settings/settings-rpc.hxx>
#include <runtime/log-output.hxx>
#include <shared/services/room/room-manager.hxx>
#include <sqlite/db-service.hxx>
#include <string>
#include <vector>
#include <sync/table-name.hxx>
#include <unistd.h>

namespace
{

constexpr double kSocketRevalidationSeconds = 60.0;
constexpr std::chrono::milliseconds kControlShutdownDeadline{2000};

Json::Value drogonConfig(const SyncDbConfig& syncDb,
                         const ListenerConfig& listener)
{
  Json::Value config = ConfigService::drogonConfig();
  if (config.isNull())
    config = Json::Value(Json::objectValue);

  Json::Value clients(Json::arrayValue);
  Json::Value client(Json::objectValue);
  client["name"] = "default";
  client["rdbms"] = "sqlite3";
  client["filename"] = syncDb.dbPath;
  client["is_fast"] = false;
  client["number_of_connections"] = 1;
  client["timeout"] = -1.0;
  clients.append(client);
  config["db_clients"] = clients;

  config["listeners"] = listenerJson(listener);

  return config;
}

}

int main()
{
  log_output::flushEachLine();
  DbService::enableUriFilenames();

  ConfigService::load("config.toml");

  try {
    DeviceFilter::requireFingerprintSecret();
  }
  catch (const std::exception& error) {
    LOG_FATAL << error.what() << " — aborting startup";
    _exit(1);
  }

  const SyncDbConfig syncDb = SyncConfig::resolveDb();
  const ListenerConfig listener = SyncConfig::resolveListener();
  SyncControlConfig control = SyncConfig::resolveControl();
  auto settingsCredentials = settingsCallers(control.callers);
  withoutSettingsCaller(control.callers);
  const SyncUpstreams upstreams = SyncConfig::resolveUpstreams();
  const int auditRetentionDays = SyncConfig::resolveAuditRetentionDays();

  const auto cameraSource =
      std::make_shared<CameraSyncGateway>(upstreams.camera);
  const auto productivitySource =
      std::make_shared<ProductivitySyncGateway>(upstreams.productivity);
  const auto notificationSource =
      std::make_shared<NotificationSyncGateway>(upstreams.notification);
  const auto identitySource = std::make_shared<IdentitySyncGateway>(
      IdentitySyncClientConfig{.target = upstreams.identity,
                               .credential = upstreams.identityCredential,
                               .fleetSecret = upstreams.identitySecret});
  const auto userDirectory = std::make_shared<CachedUserDirectory>(
      std::make_shared<IdentityUserDirectory>(),
      CachedUserDirectoryConfig{.ttl = std::chrono::seconds(10), .clock = {}});
  const auto lanes = std::make_shared<ConnectionLanes>();
  const VoiceGrpcConfig voice = VoiceGrpcConfig::resolve();
  std::shared_ptr<VoiceGrpcRelay> voiceLeg;
  if (!voice.target.empty()) {
    voiceLeg = std::make_shared<VoiceGrpcRelay>(voice, userDirectory);
    RoomManager::setSocketFarewell([relay = voiceLeg](const SocketFarewellInput& input) {
      return relay->farewell(input.conn, input.cause);
    });
  }
  const SyncHeartbeatConfig heartbeatConfig = SyncConfig::resolveHeartbeat();
  const auto presenceBoard = std::make_shared<PresenceBoard>();
  const auto heartbeatService = std::make_shared<const HeartbeatService>(
      HeartbeatService::Dependencies{.board = presenceBoard, .clock = {}},
      HeartbeatPolicy{.intervalSeconds = heartbeatConfig.intervalSeconds,
                      .graceSeconds = heartbeatConfig.graceSeconds,
                      .socketGraceSeconds = heartbeatConfig.socketGraceSeconds,
                      .guardStaleSeconds = heartbeatConfig.guardStaleSeconds});
  const SyncRegistrationStats sync =
      registerSyncSurface({.forwarder = voiceLeg,
                           .cameraSource = cameraSource,
                           .productivitySource = productivitySource,
                           .notificationSource = notificationSource,
                           .identitySource = identitySource,
                           .userDirectory = userDirectory,
                           .heartbeatSource = heartbeatService,
                           .lanes = lanes});
  sync_fan_out::onIdentityChange(
      [userDirectory, lanes](const sync_fan_out::IdentityChangeNotice& notice) {
        if (notice.table != TableName::User)
          return;
        userDirectory->forget(notice.recordId);
        lanes->revalidateUser(notice.recordId);
      });
  drogon::app().registerController(
      std::make_shared<HeartbeatController>(heartbeatService));
  drogon::app().registerController(std::make_shared<ActivityController>());
  LOG_INFO << "Sync surface registered: " << sync.controllers << " controller, "
           << sync.filters << " filters; voice leg -> "
           << (voice.target.empty() ? "unconfigured (503)"
                                    : "gRPC " + voice.target)
           << (upstreams.camera.empty()
                   ? "; camera tables -> unconfigured source (503)"
                   : "; camera tables -> gRPC " + upstreams.camera)
           << (upstreams.productivity.empty()
                   ? "; productivity leg -> unconfigured source (503)"
                   : "; productivity leg -> gRPC " + upstreams.productivity)
           << (upstreams.notification.empty()
                   ? "; notification leg -> unconfigured source (503)"
                   : "; notification leg -> gRPC " + upstreams.notification)
           << (upstreams.identity.empty()
                   ? "; identity tables -> unconfigured source (503)"
                   : "; identity tables -> gRPC " + upstreams.identity);

  const SyncRtcConfig rtc = SyncConfig::resolveRtc();
  std::shared_ptr<const RtcVoiceJoiner> rtcVoice;
  if (!voice.target.empty())
    rtcVoice = std::make_shared<VoiceRoomJoiner>(std::make_shared<VoiceClient>(
        VoiceClientConfig{.target = voice.target, .credential = voice.credential}));
  std::shared_ptr<const RtcCallClaimer> rtcCalls;
  if (!upstreams.notification.empty())
    rtcCalls = std::make_shared<NotificationCallClaimer>(std::make_shared<NotificationClient>(
        NotificationClientConfig{.target = upstreams.notification,
                                 .credential = ConfigService::getString("notifications.credential")}));
  std::shared_ptr<const LiveKitRoomClient> rtcRooms;
  if (rtc.enabled)
    rtcRooms = std::make_shared<LiveKitRoomClient>(
        LiveKitAdminConfig{.serverUrl = rtc.serverUrl, .apiKey = rtc.apiKey, .apiSecret = rtc.apiSecret});
  drogon::app().registerController(std::make_shared<RtcController>(RtcTokenServiceInput{
      .config = rtc, .voice = rtcVoice, .calls = rtcCalls, .directory = userDirectory, .rooms = rtcRooms}));
  if (rtcRooms) {
    const auto revoker = std::make_shared<RtcSessionRevoker>(rtcRooms, rtcVoice);
    sync_fan_out::onSessionEnd([revoker](const sync_fan_out::SessionEndNotice& notice) {
      revoker->sessionEnded({.userId = notice.userId, .sessionId = notice.sessionId, .cause = notice.cause});
    });
  }
  LOG_INFO << "Realtime calls: "
           << (rtc.enabled ? "LiveKit API " + rtc.serverUrl + ", apps dial port " +
                                 std::to_string(rtc.publicPort)
                           : std::string("unconfigured (/rtc/token answers 503)"));

  drogon::app().loadConfigJson(drogonConfig(syncDb, listener));
  certificate_reload::watch(listener);

  drogon::app().registerPreRoutingAdvice(
      [](const drogon::HttpRequestPtr& req, drogon::AdviceCallback&& cb,
         drogon::AdviceChainCallback&& chain) {
        if (req->method() == drogon::Options) {
          Cors::handleOptions(req, std::move(cb));
          return;
        }
        chain();
      });
  drogon::app().registerPostHandlingAdvice(
      [](const drogon::HttpRequestPtr&, const drogon::HttpResponsePtr& resp) {
        Cors::apply(resp);
      });

  drogon::app().setExceptionHandler(ErrorHandler::handleException);

  drogon::app().setCustomErrorHandler(
      [](drogon::HttpStatusCode code, const drogon::HttpRequestPtr&) {
        return ErrorHandler::unmatchedRoute(code);
      });

  AuditFanOut auditFanOut;
  AuditRetentionService auditRetention;
  const std::string natsUrl = ConfigService::getString("nats.url");
  std::shared_ptr<NatsBus> natsBus;
  std::shared_ptr<NotificationDeliveryConsumer> deliveryConsumer;
  std::shared_ptr<ChangeFeedConsumer> changeFeedConsumer;
  std::unique_ptr<NatsPushIntentSink> heartbeatPush;
  std::unique_ptr<HeartbeatFeed> heartbeatFeed;
  if (natsUrl.empty()) {
    LOG_INFO << "NATS not configured; event bus disabled";
  }
  else {
    natsBus = std::make_shared<NatsBus>();
    const bool connected = natsBus->connect();
    changeFeedConsumer = std::make_shared<ChangeFeedConsumer>(
        ChangeFeedConsumer::Dependencies{.bus = natsBus.get(),
                                         .auditFanOut = &auditFanOut},
        ChangeFeedConsumer::Config{.feeds = change_feed::defaults(),
                                   .maxDeliver = 10});
    deliveryConsumer = std::make_shared<NotificationDeliveryConsumer>(
        NotificationDeliveryConsumer::Dependencies{.bus = natsBus.get(),
                                                   .dispatch = {}},
        NotificationDeliveryConsumer::Config{
            .stream = std::string(nats_subject::kNotificationDeliveryStream),
            .durable = "argus-sync-delivery",
            .subject = std::string(nats_subject::kNotificationDelivery),
            .maxDeliver = 10,
            .poisonMaxAttempts = 3});
    if (push_intent::enabledFromConfig())
      heartbeatPush = std::make_unique<NatsPushIntentSink>(natsBus);
    heartbeatFeed = std::make_unique<HeartbeatFeed>(
        HeartbeatFeed::Dependencies{
            .bus = natsBus.get(),
            .board = presenceBoard,
            .heartbeat = heartbeatService,
            .directory = heartbeatConfig.presenceTarget.empty()
                             ? nullptr
                             : std::make_shared<const GuardPresenceDirectory>(
                                   std::make_shared<const GuardPresenceClient>(GuardPresenceClientConfig{
                                       .target = heartbeatConfig.presenceTarget,
                                       .credential = heartbeatConfig.presenceCredential})),
            .push = heartbeatPush.get(),
            .emit = [](int64_t userId, std::string_view frame) {
              RoomManager{}.emit(userRoom(userId), frame);
            }},
        HeartbeatFeed::Config{
            .presenceSubject = std::string(nats_subject::kGuardPresenceChanged),
            .guardHeartbeatSubject = std::string(nats_subject::kGuardHeartbeat),
            .pushIntervalSeconds =
                static_cast<double>(heartbeatConfig.pushIntervalSeconds),
            .refillSeconds = static_cast<double>(heartbeatConfig.refillSeconds)});
    if (connected)
      LOG_INFO << "NATS event bus connected to " << natsBus->options().url;
    else
      LOG_WARN << "NATS unavailable at " << natsUrl
               << "; subscriptions stay pending until reconnected";
  }

  const std::weak_ptr<NatsBus> healthBus = natsBus;
  drogon::app().registerController(std::make_shared<HealthController>(
      HealthStatus{.serviceName = "argus-sync",
                   .extras = {{"nats", [healthBus]() {
                                 Json::Value status(Json::objectValue);
                                 const auto bus = healthBus.lock();
                                 if (!bus) {
                                   status["enabled"] = false;
                                   status["connected"] = false;
                                   return status;
                                 }
                                 status["enabled"] = true;
                                 status["connected"] = bus->isConnected();
                                 return status;
                               }}}}));

  const auto controlGate = std::make_shared<const argus::client::FleetCallerGate>(
      argus::client::FleetGateConfig{
          .expectedCallers = sync_control_callers::expected(),
          .callerPairs = control.callers,
          .legacySecret = control.secret,
          .onFirstLegacy = [](const std::vector<std::string>& unpaired) {
            LOG_WARN << "Sync control RPC: a caller presented the fleet-wide "
                        "[sync] control_secret; it reaches only what an "
                        "unpaired caller may call until every caller has its "
                        "own [rpc.callers] credential (unpaired: "
                     << unpaired.size() << ", run scripts/setup.sh or "
                        "scripts/provision-host.sh to pair them)";
          }});
  if (control.reachableBeyondLoopback() && controlGate->open()) {
    LOG_FATAL
        << "[server] host " << control.listener.host
        << " is reachable beyond loopback and injects frames into any "
           "user's room: pair its callers in [rpc.callers] — aborting startup";
    _exit(1);
  }

  SyncControlRpcService controlRpc(controlGate);
  SyncModuleData moduleData;
  SettingsRegistry noSettings({});
  std::unique_ptr<SettingsRpcService> settingsRpc;
  grpc::ServerBuilder controlBuilder;
  controlBuilder.AddListeningPort(control.listener.host + ":" +
                                      std::to_string(control.listener.port),
                                  grpc::InsecureServerCredentials());
  controlBuilder.RegisterService(&controlRpc);
  if (settingsCredentials.empty()) {
    LOG_INFO << "Module data RPC not served: [rpc.callers] settings is empty";
  }
  else {
    settingsRpc = std::make_unique<SettingsRpcService>(SettingsRpcInput{
        .service = "sync", .registry = &noSettings, .credentials = std::move(settingsCredentials)});
    settingsRpc->attachModuleData(moduleData);
    controlBuilder.RegisterService(settingsRpc.get());
  }
  std::unique_ptr<grpc::Server> controlServer(controlBuilder.BuildAndStart());
  if (controlServer)
    LOG_INFO << "Sync control RPC listening on " << control.listener.host << ":"
             << control.listener.port << " (cleartext, "
             << (controlGate->open()
                     ? "loopback only, no caller credential"
                     : std::to_string(controlGate->pairedCount()) +
                           " paired callers" +
                           (controlGate->acceptsLegacy()
                                ? ", legacy fleet secret for the unpaired ones"
                                : ""))
             << ")";
  else
    LOG_WARN << "Sync control RPC failed to listen on " << control.listener.host
             << ":" << control.listener.port
             << "; role changes and disconnects cannot reach this service";
  argus::client::GrpcServerDrain controlDrain(std::move(controlServer), kControlShutdownDeadline);

  LOG_INFO << "Listening on " << listener.host << ":" << listener.port
           << (listener.tls ? " (TLS" : " (plain") << ", cert "
           << listener.certPath << "); sync database " << syncDb.dbPath;

  RoomManager roomManagerLifecycle;
  roomManagerLifecycle.init();

  drogon::app().registerBeginningAdvice([&syncDb = syncDb, &auditFanOut,
                                         &changeFeedConsumer,
                                         &deliveryConsumer, &auditRetention,
                                         &heartbeatFeed, &lanes, &userDirectory,
                                         auditRetentionDays]() {
    if (!auditFanOut.migrateLegacySchema() ||
        !DbService::runScriptFile(syncDb.schemaPath) ||
        !auditFanOut.backfillActivityModules()) {
      LOG_FATAL << "Sync database schema failed to apply — aborting startup";
      _exit(1);
    }

    DbService::applyPragmas();

    if (changeFeedConsumer)
      changeFeedConsumer->start();
    if (deliveryConsumer)
      deliveryConsumer->start();
    auditRetention.start(auditRetentionDays);
    if (heartbeatFeed)
      heartbeatFeed->start();
    lanes->startRevalidation({.intervalSeconds = kSocketRevalidationSeconds,
                              .directory = userDirectory});
  });

  shutdown_signal::onStop(shutdown_signal::drainOf(*lanes, "sync-sockets"));
  if (changeFeedConsumer)
    shutdown_signal::onStop(
        shutdown_signal::drainOf(*changeFeedConsumer, "sync-change-feed"));
  if (deliveryConsumer)
    shutdown_signal::onStop(
        shutdown_signal::drainOf(*deliveryConsumer, "sync-delivery"));
  if (heartbeatFeed)
    shutdown_signal::onStop(
        shutdown_signal::drainOf(*heartbeatFeed, "sync-heartbeat"));
  if (voiceLeg)
    shutdown_signal::onStop(shutdown_signal::drainOf(*voiceLeg, "sync-voice"));
  shutdown_signal::onStop(
      shutdown_signal::drainOf(auditRetention, "sync-audit-retention"));
  shutdown_signal::onStop(
      shutdown_signal::drainOf(controlDrain, "sync-control-rpc"));

  shutdown_signal::onQuit(
      [dbPath = syncDb.dbPath] { DbService::freezeClient(dbPath); });

  std::unique_ptr<MdnsService> mdnsService;
  drogon::app().registerBeginningAdvice([&mdnsService, &listener]() {
    mdnsService = std::make_unique<MdnsService>(
        routeAnnouncements({.port = listener.port, .tls = listener.tls}));
    mdnsService->initialize();
  });

  drogon::app().setThreadNum(0).run();

  controlDrain.stop();
  return 0;
}
