#include <drogon/drogon.h>
#include <feature/camera-notification/services/camera-object-notifier.hxx>
#include <app/rpc/call-rpc-service.hxx>
#include <app/rpc/notification-rpc-service.hxx>
#include <feature/call/controllers/call-preference-controller.hxx>
#include <feature/call/infra/identity-call-directory.hxx>
#include <feature/call/infra/notification-call-sink.hxx>
#include <feature/call/infra/sync-call-signal.hxx>
#include <feature/call/infra/voice-call-announcer.hxx>
#include <feature/call/services/call-feed.hxx>
#include <feature/settings/notification-settings.hxx>
#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/role-filter.hxx>
#include <auth/valid-json-filter.hxx>
#include <grpcpp/grpcpp.h>
#include <http/cors.hxx>
#include <http/certificate-reload.hxx>
#include <http/error-handler.hxx>
#include <http/health-controller.hxx>
#include <http/listener-config.hxx>
#include <http/route-announcements.hxx>
#include <identity/identity-client.hxx>
#include <mdns/mdns-service.hxx>
#include <shared/services/change-sink/nats-notification-change-sink.hxx>
#include <shared/services/delivery-sink/nats-notification-delivery-sink.hxx>
#include <nats/nats-bus.hxx>
#include <nats/nats-push-intent-sink.hxx>
#include <nats/nats-subject.hxx>
#include <runtime/shutdown-signal.hxx>
#include <settings/settings-rpc.hxx>
#include <runtime/log-output.hxx>
#include <config/notification-config.hxx>
#include <sync/sync-client.hxx>
#include <sync/user-change-sink.hxx>
#include <config/config-service.hxx>
#include <sqlite/db-service.hxx>
#include <unistd.h>

#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace
{

Json::Value drogonConfig(const NotificationDbConfig& notificationDb,
                         const ListenerConfig& listener)
{
  Json::Value config = ConfigService::drogonConfig();
  if (config.isNull())
    config = Json::Value(Json::objectValue);

  Json::Value clients(Json::arrayValue);
  Json::Value client(Json::objectValue);
  client["name"] = "default";
  client["rdbms"] = "sqlite3";
  client["filename"] = notificationDb.dbPath;
  client["is_fast"] = false;
  client["number_of_connections"] = 1;
  client["timeout"] = -1.0;
  clients.append(client);
  config["db_clients"] = std::move(clients);

  config["listeners"] = listenerJson(listener);

  return config;
}

}

int main()
{
  log_output::flushEachLine();
  DbService::enableUriFilenames();

  ConfigService::load("config.toml");

  const NotificationDbConfig notificationDb = NotificationConfig::resolveDb();
  const ListenerConfig listener = NotificationConfig::resolveListener();
  const GrpcListenerConfig grpcListener =
      NotificationConfig::resolveRpcListener();

  drogon::app().registerFilter(std::make_shared<DeviceFilter>());
  drogon::app().registerFilter(std::make_shared<ValidJsonFilter>());
  drogon::app().registerFilter(std::make_shared<JwtFilter>());
  drogon::app().registerFilter(std::make_shared<RoleFilter>());

  drogon::app().loadConfigJson(drogonConfig(notificationDb, listener));
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

  LOG_INFO << "Listening on " << listener.host << ":" << listener.port
           << (listener.tls ? " (TLS" : " (plain") << ", cert "
           << listener.certPath << "); notification database "
           << notificationDb.dbPath << "; gRPC NotificationService on "
           << grpcListener.host << ":" << grpcListener.port;

  std::shared_ptr<NatsNotificationChangeSink> changeSink;
  std::shared_ptr<NatsNotificationDeliverySink> deliverySink;
  std::shared_ptr<NatsBus> natsBus;
  std::shared_ptr<NatsPushIntentSink> pushIntentSink;
  const std::string natsUrl = ConfigService::getString("nats.url");
  if (natsUrl.empty()) {
    LOG_INFO << "NATS not configured; change funnel and delivery reconciler "
                "disabled, intents stay pending";
  }
  else {
    natsBus = std::make_shared<NatsBus>();
    if (natsBus->connect())
      LOG_INFO << "NATS event bus connected to " << natsBus->options().url;
    else
      LOG_WARN << "NATS unavailable at " << natsUrl
               << "; bus reconnects in background, changes retained locally";
    changeSink = std::make_shared<NatsNotificationChangeSink>(
        natsBus, NatsNotificationChangeSink::Config{});
    user_change::setNotificationSink(changeSink.get());
    deliverySink = std::make_shared<NatsNotificationDeliverySink>(
        natsBus, NatsNotificationDeliverySink::Config{
                     .stream = std::string(
                         nats_subject::kNotificationDeliveryStream),
                     .subject = std::string(
                         nats_subject::kNotificationDelivery)});
  }

  if (push_intent::enabledFromConfig()) {
    if (natsBus) {
      pushIntentSink = std::make_shared<NatsPushIntentSink>(natsBus);
      push_intent::setSink(pushIntentSink.get());
      LOG_INFO << "Push intents enabled (" << nats_subject::kNotificationPushIntent
               << ")";
    } else {
      LOG_WARN << "[push] enabled but NATS unavailable; push intents disabled";
    }
  }

  const NotificationService::Dependencies deliveryDeps{
      .deliverySink = deliverySink,
      .pushSink = pushIntentSink,
      .pushRequired = push_intent::enabledFromConfig()};

  const NotificationIdentityConfig identityConfig =
      NotificationConfig::resolveIdentity();
  std::shared_ptr<IdentityClient> identityClient;
  if (identityConfig.target.empty()) {
    LOG_WARN << "Identity target unconfigured; camera notifications keep "
                "their fallback record but reach no recipient, and calls "
                "greet nobody by name";
  }
  else {
    identityClient = std::make_shared<IdentityClient>(identityConfig.target,
                                                      identityConfig.rpcSecret);
  }

  std::shared_ptr<CameraObjectNotifier> cameraNotifier;
  if (natsBus) {
    cameraNotifier = std::make_shared<CameraObjectNotifier>(
        camera_notifier::resolveConfig(),
        CameraNotifierDependencies{.identityClient = identityClient,
                                   .delivery = deliveryDeps});
    camera_notifier::subscribe(*natsBus, *cameraNotifier);
    LOG_INFO << "Camera object fallback subscribed on "
             << nats_subject::kCameraObjectDetected;
  }

  const NotificationSyncControlConfig syncControl =
      NotificationConfig::resolveSyncControl();
  std::shared_ptr<const CallSignal> callSignal;
  if (syncControl.target.empty()) {
    LOG_WARN << "Sync control target unconfigured; calls cannot ring the app "
                "and reach users only by push and missed-call notifications";
  }
  else {
    callSignal = std::make_shared<SyncCallSignal>(std::make_shared<SyncClient>(
        SyncClientConfig{.target = syncControl.target,
                         .fleetSecret = syncControl.secret}));
  }
  const NotificationVoiceConfig voiceConfig = NotificationConfig::resolveVoice();
  std::shared_ptr<const LiveCallAnnouncer> callAnnouncer;
  if (!voiceConfig.target.empty() && !voiceConfig.credential.empty())
    callAnnouncer = std::make_shared<VoiceCallAnnouncer>(std::make_shared<VoiceClient>(
        VoiceClientConfig{.target = voiceConfig.target,
                          .credential = voiceConfig.credential}));
  else
    LOG_INFO << "Voice target or credential unset; calls never speak into a "
                "live conversation and always ring";
  const auto callEngine = std::make_shared<CallEngine>(
      NotificationConfig::resolveCalls(),
      CallEngineDependencies{
          .signal = callSignal,
          .announcer = callAnnouncer,
          .directory = identityClient
                           ? std::make_shared<IdentityCallDirectory>(identityClient)
                           : nullptr,
          .notifier = std::make_shared<NotificationCallSink>(deliveryDeps),
          .push = pushIntentSink,
          .clock = {},
          .localHour = {},
          .blockingOffLoop = true});
  drogon::app().registerController(std::make_shared<CallPreferenceController>());
  if (natsBus) {
    call_feed::subscribe(*natsBus, callEngine);
    LOG_INFO << "Call engine subscribed on " << nats_subject::kGuardKnownSeen;
  }
  call_feed::startSweep(callEngine);

  const std::weak_ptr<NatsBus> healthBus = natsBus;
  drogon::app().registerController(std::make_shared<HealthController>(
      HealthStatus{
          .serviceName = "argus-notification",
          .extras = {{"nats",
                      [healthBus]() {
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
                      }},
                     {"notifications_fallback",
                      [cameraNotifier]() {
                        Json::Value status(Json::objectValue);
                        if (cameraNotifier == nullptr) {
                          status["subscribed"] = false;
                          return status;
                        }
                        status["subscribed"] = true;
                        const auto counts =
                            cameraNotifier->policy().fallbackCounts();
                        status["passed"] = static_cast<Json::Int64>(counts.passed);
                        status["dropped_known"] =
                            static_cast<Json::Int64>(counts.droppedKnown);
                        status["dropped_weak_score"] =
                            static_cast<Json::Int64>(counts.droppedWeakScore);
                        status["dropped_short_dwell"] =
                            static_cast<Json::Int64>(counts.droppedShortDwell);
                        return status;
                      }}}}));

  drogon::app().registerBeginningAdvice([&notificationDb, &changeSink]() {
    if (!DbService::runScriptFile(notificationDb.schemaPath)) {
      LOG_FATAL
          << "Notification database schema failed to apply — aborting startup";
      _exit(1);
    }

    const auto hasColumn = [](const std::string& table,
                              const std::string& column) {
      const auto rows = DbService::client()->execSqlSync(
          "SELECT COUNT(*) AS total FROM pragma_table_info('" + table +
              "') WHERE name = ?",
          column);
      return !rows.empty() && rows.front()["total"].as<int>() > 0;
    };
    if (!hasColumn("notification_command", "expected_count"))
      DbService::client()->execSqlSync(
          "ALTER TABLE notification_command ADD COLUMN expected_count INTEGER "
          "NOT NULL DEFAULT 0");
    if (!hasColumn("notification_command", "fingerprint"))
      DbService::client()->execSqlSync(
          "ALTER TABLE notification_command ADD COLUMN fingerprint TEXT NOT "
          "NULL DEFAULT ''");
    if (!hasColumn("notification_delivery", "acked_at"))
      DbService::client()->execSqlSync(
          "ALTER TABLE notification_delivery ADD COLUMN acked_at INTEGER NOT "
          "NULL DEFAULT 0");
    if (!hasColumn("notification_delivery", "created_ms"))
      DbService::client()->execSqlSync(
          "ALTER TABLE notification_delivery ADD COLUMN created_ms INTEGER "
          "NOT NULL DEFAULT 0");
    if (!hasColumn("notification_delivery", "sent_ms"))
      DbService::client()->execSqlSync(
          "ALTER TABLE notification_delivery ADD COLUMN sent_ms INTEGER NOT "
          "NULL DEFAULT 0");
    if (!hasColumn("notification_delivery", "acked_ms"))
      DbService::client()->execSqlSync(
          "ALTER TABLE notification_delivery ADD COLUMN acked_ms INTEGER NOT "
          "NULL DEFAULT 0");

    DbService::applyPragmas();
    DbService::client()->execSqlSync("PRAGMA foreign_keys = OFF");

    if (changeSink) {
      changeSink->reconcile();
    }
  });

  NotificationRpcService notificationRpc(deliveryDeps);
  notificationRpc.attachCallEngine(callEngine);
  CallRpcService callRpc(callEngine, NotificationConfig::resolveCallCallers());
  SettingsRegistry settings(notificationSettingsCatalog());
  settings.onChange([cameraNotifier, callEngine](const std::vector<std::string>&) {
    if (cameraNotifier)
      camera_notifier::refresh(*cameraNotifier);
    callEngine->reconfigure(NotificationConfig::resolveCalls());
  });
  std::unique_ptr<SettingsRpcService> settingsRpc;
  if (auto callers = NotificationConfig::resolveSettingsCallers();
      !callers.empty())
    settingsRpc = std::make_unique<SettingsRpcService>(
        SettingsRpcInput{.service = "notification",
                         .registry = &settings,
                         .credentials = std::move(callers)});

  grpc::ServerBuilder grpcBuilder;
  const std::string grpcAddress =
      grpcListener.host + ":" + std::to_string(grpcListener.port);
  grpcBuilder.AddListeningPort(grpcAddress, grpc::InsecureServerCredentials());
  grpcBuilder.RegisterService(&notificationRpc);
  grpcBuilder.RegisterService(&callRpc);
  if (settingsRpc)
    grpcBuilder.RegisterService(settingsRpc.get());
  std::unique_ptr<grpc::Server> grpcServer(grpcBuilder.BuildAndStart());
  if (!grpcServer) {
    LOG_FATAL << "gRPC server failed to listen on " << grpcAddress;
    return 1;
  }

  notificationRpc.startDeliveryReconciler();
  notificationRpc.startSelfTestProber();

  shutdown_signal::onQuit(
      [dbPath = notificationDb.dbPath] { DbService::freezeClient(dbPath); });
  if (changeSink) {
    shutdown_signal::onStop(
        shutdown_signal::drainOf(*changeSink, "notification-change"));
  }

  std::unique_ptr<MdnsService> mdnsService;
  drogon::app().registerBeginningAdvice([&mdnsService, &listener]() {
    mdnsService = std::make_unique<MdnsService>(
        routeAnnouncements({.port = listener.port, .tls = listener.tls}));
    mdnsService->initialize();
  });

  drogon::app()
      .setThreadNum(0)
      .run();

  grpcServer->Shutdown();
  return 0;
}
