#include <config/app-config.hxx>
#include <controllers/health-controller.hxx>
#include <drogon/drogon.h>
#include <filter/device/device-filter.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <filter/role/role-filter.hxx>
#include <filter/valid-json/valid-json-filter.hxx>
#include <notification/nats-notification-change-sink.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>
#include <shared/wrapper/nats/nats-push-intent-sink.hxx>
#include <shared/wrapper/nats/nats-subject.hxx>
#include <notification/notification-config.hxx>
#include <server/listener-config.hxx>
#include <shared/contracts/user-change-sink.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <thread>

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

// The JWT filter resolves the caller's user row (and the bound refresh-token
// session) from identity.db: without the named identity client those reads
// would fall back to notification.db, which holds no user table, and every
// authenticated request would 401. Same install as argus-productivity (Ruling
// AM); with no [identity] db configured the fallback keeps this boot
// identity-free.
void installIdentityClient()
{
  const auto path = ConfigService::getString("identity.db");
  if (path.empty())
    return;

  // The gateway creates identity.db at its own boot, which on a fresh
  // install may land after ours; wait bounded before opening it read-only.
  for (int ms = 0; ms < 30000 && !std::filesystem::exists(path); ms += 250)
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

  try {
    const auto identity = drogon::orm::DbClient::newSqlite3Client(
        "filename=file:" + path + "?mode=ro", 1);
    identity->execSqlSync("PRAGMA busy_timeout = 5000");
    DbService::setIdentityClient(identity);
    LOG_INFO << "Identity database opened read-only: " << path;
  }
  catch (const std::exception& e) {
    LOG_WARN << "Identity database open failed (" << e.what()
             << "); identity reads fall back to the default client";
  }
}

} // namespace

int main()
{
  DbService::enableUriFilenames();

  ConfigService::load("config.toml");

  installIdentityClient();

  const NotificationDbConfig notificationDb = NotificationConfig::resolveDb();
  const ListenerConfig listener = ListenerConfig::resolve();

  drogon::app().registerController(std::make_shared<HealthController>());

  drogon::app().registerFilter(std::make_shared<DeviceFilter>());
  drogon::app().registerFilter(std::make_shared<ValidJsonFilter>());
  drogon::app().registerFilter(std::make_shared<JwtFilter>());
  drogon::app().registerFilter(std::make_shared<RoleFilter>());

  drogon::app().loadConfigJson(drogonConfig(notificationDb, listener));

  // The legacy answers CORS preflight for every path in pre-routing, and the
  // gateway forwards OPTIONS on proxied paths untouched, so this surface
  // keeps answering them itself.
  drogon::app().registerPreRoutingAdvice(
      [](const drogon::HttpRequestPtr& req, drogon::AdviceCallback&& cb,
         drogon::AdviceChainCallback&& chain) {
        if (req->method() == drogon::Options) {
          AppConfig::handleOptions(req, std::move(cb));
          return;
        }
        chain();
      });
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

  LOG_INFO << "Listening on " << listener.host << ":" << listener.port
           << " (plain); notification database " << notificationDb.dbPath;

  drogon::app().registerBeginningAdvice([&notificationDb]() {
    if (!DbService::runScriptFile(notificationDb.schemaPath)) {
      LOG_FATAL
          << "Notification database schema failed to apply — aborting startup";
      _exit(1);
    }

    DbService::applyPragmas();
    // The notification tables reference user rows that live in identity.db,
    // so foreign-key enforcement stays off on every connection (Ruling AN).
    DbService::client()->execSqlSync("PRAGMA foreign_keys = OFF");
  });

  // The notification-domain change funnel (Rulings AQ/Y): without NATS
  // configured the feature services drop their change events with a warning,
  // which keeps a NATS-less service bootable for contract tests.
  std::shared_ptr<NatsNotificationChangeSink> changeSink;
  std::shared_ptr<NatsBus> natsBus;
  const std::string natsUrl = ConfigService::getString("nats.url");
  if (natsUrl.empty()) {
    LOG_INFO << "NATS not configured; notification change funnel disabled";
  }
  else {
    natsBus = std::make_shared<NatsBus>();
    if (natsBus->connect()) {
      LOG_INFO << "NATS event bus connected to " << natsBus->options().url;
      changeSink = std::make_shared<NatsNotificationChangeSink>(natsBus);
      user_change::setNotificationSink(changeSink.get());
    }
    else {
      natsBus.reset();
      LOG_WARN << "NATS unavailable at " << natsUrl
               << "; notification change funnel disabled";
    }
  }

  std::shared_ptr<NatsPushIntentSink> pushIntentSink;
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

  drogon::app()
      .setThreadNum(0)
      .run();
  return 0;
}
