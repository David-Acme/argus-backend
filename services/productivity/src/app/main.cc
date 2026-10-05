#include <feature/agenda/infra/notification-agenda-notifier.hxx>
#include <feature/agenda/services/agenda-announcer.hxx>
#include <feature/agenda/services/agenda-sweeper.hxx>
#include <app/rpc/productivity-rpc-server.hxx>
#include <drogon/drogon.h>
#include <feature/sync/productivity-sync-rpc-service.hxx>
#include <feature/calendar-event/controllers/calendar-event-controller.hxx>
#include <feature/calendar-event-share/controllers/calendar-event-share-controller.hxx>
#include <feature/project/controllers/project-controller.hxx>
#include <feature/project-member/controllers/project-member-controller.hxx>
#include <feature/project-task/controllers/project-task-controller.hxx>
#include <auth/device-filter.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/role-filter.hxx>
#include <auth/valid-json-filter.hxx>
#include <grpcpp/grpcpp.h>
#include <http/cors.hxx>
#include <http/error-handler.hxx>
#include <http/health-controller.hxx>
#include <http/certificate-reload.hxx>
#include <http/listener-config.hxx>
#include <http/route-announcements.hxx>
#include <mdns/mdns-service.hxx>
#include <config/productivity-config.hxx>
#include <shared/services/change-sink/nats-productivity-change-sink.hxx>
#include <nats/nats-bus.hxx>
#include <runtime/shutdown-signal.hxx>
#include <runtime/log-output.hxx>
#include <sync/user-change-sink.hxx>
#include <config/config-service.hxx>
#include <sqlite/db-service.hxx>
#include <unistd.h>

#include <memory>
#include <string>

namespace
{

Json::Value drogonConfig(const ProductivityDbConfig& productivityDb,
                         const ListenerConfig& listener)
{
  Json::Value config = ConfigService::drogonConfig();
  if (config.isNull())
    config = Json::Value(Json::objectValue);

  Json::Value clients(Json::arrayValue);
  Json::Value client(Json::objectValue);
  client["name"] = "default";
  client["rdbms"] = "sqlite3";
  client["filename"] = productivityDb.dbPath;
  client["is_fast"] = false;
  client["number_of_connections"] = 1;
  client["timeout"] = -1.0;
  clients.append(client);
  config["db_clients"] = clients;

  config["listeners"] = listenerJson(listener);

  return config;
}

bool declaresForeignUserReferences()
{
  const auto rows = DbService::productivityClient()->execSqlSync(
      "SELECT COUNT(*) AS total FROM sqlite_master AS m, "
      "pragma_foreign_key_list(m.name) AS f "
      "WHERE m.type = 'table' AND f.\"table\" = 'user'");
  return !rows.empty() && rows.front()["total"].as<int64_t>() > 0;
}

constexpr double kAgendaSweepPeriodS = 30.0;

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

  const ProductivityDbConfig productivityDb = ProductivityConfig::resolveDb();
  const ListenerConfig listener = ProductivityConfig::resolveListener();

  ProductivitySyncRpcService productivitySyncRpc;

  ProductivityRpcServer rpc({.services = {&productivitySyncRpc}});
  if (!rpc.listening()) {
    LOG_FATAL << "gRPC server failed to listen on " << rpc.address();
    return 1;
  }

  drogon::app().registerController(std::make_shared<CalendarEventController>());
  drogon::app().registerController(
      std::make_shared<CalendarEventShareController>());
  drogon::app().registerController(std::make_shared<ProjectController>());
  drogon::app().registerController(std::make_shared<ProjectMemberController>());
  drogon::app().registerController(std::make_shared<ProjectTaskController>());

  drogon::app().registerController(std::make_shared<HealthController>(HealthStatus{.serviceName = "argus-productivity", .extras = {}}));

  drogon::app().registerFilter(std::make_shared<DeviceFilter>());
  drogon::app().registerFilter(std::make_shared<ValidJsonFilter>());
  drogon::app().registerFilter(std::make_shared<JwtFilter>());
  drogon::app().registerFilter(std::make_shared<RoleFilter>());

  drogon::app().loadConfigJson(drogonConfig(productivityDb, listener));
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
           << listener.certPath << "); productivity database "
           << productivityDb.dbPath << "; gRPC SyncService on " << rpc.address();

  std::shared_ptr<NatsProductivityChangeSink> changeSink;
  std::shared_ptr<NatsBus> natsBus;
  const std::string natsUrl = ConfigService::getString("nats.url");
  if (natsUrl.empty()) {
    LOG_INFO << "NATS not configured; productivity change funnel disabled";
  }
  else {
    natsBus = std::make_shared<NatsBus>();
    if (natsBus->connect())
      LOG_INFO << "NATS event bus connected to " << natsBus->options().url;
    else
      LOG_WARN << "NATS unavailable at " << natsUrl
               << "; bus reconnects in background, changes retained locally";
    changeSink = std::make_shared<NatsProductivityChangeSink>(
        natsBus, NatsProductivityChangeSink::Config{});
    user_change::setProductivitySink(changeSink.get());
  }

  drogon::app().registerBeginningAdvice([&productivityDb, &changeSink]() {
    if (!DbService::runScriptFile(productivityDb.schemaPath,
                                  DbService::productivityClient())) {
      LOG_FATAL
          << "Productivity database schema failed to apply — aborting startup";
      _exit(1);
    }

    DbService::applyPragmas(DbService::productivityClient());
    if (declaresForeignUserReferences()) {
      DbService::productivityClient()->execSqlSync("PRAGMA foreign_keys = OFF");
      LOG_WARN << "productivity.db still declares REFERENCES user(id); foreign "
                  "keys stay off until the database is rebuilt from "
                  "database/schema.sql (services/productivity/CONTEXT.md)";
    }

    if (changeSink) {
      changeSink->reconcile();
    }
  });

  const ProductivityNotificationConfig notifications =
      ProductivityConfig::resolveNotifications();
  const ProductivityAgendaConfig agendaConfig = ProductivityConfig::resolveAgenda();
  std::shared_ptr<const AgendaNotifier> agendaNotifier;
  if (!notifications.target.empty() && !notifications.credential.empty())
    agendaNotifier = std::make_shared<NotificationAgendaNotifier>(
        std::make_shared<NotificationClient>(NotificationClientConfig{
            .target = notifications.target,
            .credential = notifications.credential}));
  const auto agenda = std::make_shared<AgendaAnnouncer>(
      AgendaAnnouncerConfig{.enabled = agendaConfig.enabled,
                            .graceS = agendaConfig.graceS,
                            .retentionS = 2592000},
      AgendaAnnouncerDependencies{.notifier = agendaNotifier,
                                  .clock = {},
                                  .blockingOffLoop = true});
  const auto agendaSweeper = std::make_shared<AgendaSweeper>(agenda);
  if (agenda->enabled()) {
    agendaSweeper->start(kAgendaSweepPeriodS);
    LOG_INFO << "Agenda announcements on (each user's lead time), through "
             << notifications.target;
  }
  else {
    LOG_INFO << "Agenda announcements off (agenda.enabled or the notification "
                "target/credential is unset)";
  }

  shutdown_signal::onQuit(
      [dbPath = productivityDb.dbPath] { DbService::freezeClient(dbPath); });
  if (changeSink) {
    shutdown_signal::onStop(
        shutdown_signal::drainOf(*changeSink, "productivity-change"));
  }
  shutdown_signal::onStop(
      shutdown_signal::drainOf(*agendaSweeper, "productivity-agenda"));
  shutdown_signal::onStop(shutdown_signal::drainOf(rpc, "productivity-grpc"));

  std::unique_ptr<MdnsService> mdnsService;
  drogon::app().registerBeginningAdvice([&mdnsService, &listener]() {
    mdnsService = std::make_unique<MdnsService>(
        routeAnnouncements({.port = listener.port, .tls = listener.tls}));
    mdnsService->initialize();
  });

  drogon::app()
      .setThreadNum(0)
      .run();

  rpc.shutdown();
  return 0;
}
