#include <drogon/drogon.h>
#include <feature/rpc/notification-rpc-service.hxx>
#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/role-filter.hxx>
#include <auth/valid-json-filter.hxx>
#include <grpcpp/grpcpp.h>
#include <http/cors.hxx>
#include <http/error-handler.hxx>
#include <http/health-controller.hxx>
#include <http/listener-config.hxx>
#include <notification/nats-notification-change-sink.hxx>
#include <notification/nats-notification-delivery-sink.hxx>
#include <nats/nats-bus.hxx>
#include <nats/nats-push-intent-sink.hxx>
#include <nats/nats-subject.hxx>
#include <runtime/shutdown-signal.hxx>
#include <notification/notification-config.hxx>
#include <sync/user-change-sink.hxx>
#include <config/config-service.hxx>
#include <sqlite/db-service.hxx>
#include <unistd.h>

#include <memory>
#include <string>

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
  config["db_clients"] = clients;

  config["listeners"] = listenerJson(listener);

  return config;
}

}

int main()
{
  DbService::enableUriFilenames();

  ConfigService::load("config.toml");

  const NotificationDbConfig notificationDb = NotificationConfig::resolveDb();
  const ListenerConfig listener = ListenerConfig::resolve(7028);
  const GrpcListenerConfig grpcListener = GrpcListenerConfig::resolve(7038);

  drogon::app().registerController(std::make_shared<HealthController>(HealthStatus{.serviceName = "argus-notification", .extras = {}}));

  drogon::app().registerFilter(std::make_shared<DeviceFilter>());
  drogon::app().registerFilter(std::make_shared<ValidJsonFilter>());
  drogon::app().registerFilter(std::make_shared<JwtFilter>());
  drogon::app().registerFilter(std::make_shared<RoleFilter>());

  drogon::app().loadConfigJson(drogonConfig(notificationDb, listener));

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
            << " (plain); notification database " << notificationDb.dbPath
            << "; gRPC NotificationService on " << grpcListener.host << ":"
            << grpcListener.port;

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

  NotificationRpcService notificationRpc(
      {.deliverySink = deliverySink,
       .pushSink = pushIntentSink,
       .pushRequired = push_intent::enabledFromConfig()});

  grpc::ServerBuilder grpcBuilder;
  const std::string grpcAddress =
      grpcListener.host + ":" + std::to_string(grpcListener.port);
  grpcBuilder.AddListeningPort(grpcAddress, grpc::InsecureServerCredentials());
  grpcBuilder.RegisterService(&notificationRpc);
  std::unique_ptr<grpc::Server> grpcServer(grpcBuilder.BuildAndStart());
  if (!grpcServer) {
    LOG_FATAL << "gRPC server failed to listen on " << grpcAddress;
    return 1;
  }

  notificationRpc.startDeliveryReconciler();
  notificationRpc.startSelfTestProber();

  if (changeSink) {
    shutdown_signal::onStop(
        shutdown_signal::drainOf(*changeSink, "notification-change"));
  }

  drogon::app()
      .setThreadNum(0)
      .run();

  grpcServer->Shutdown();
  return 0;
}
