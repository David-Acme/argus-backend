#include <config/app-config.hxx>
#include <controllers/health-controller.hxx>
#include <drogon/drogon.h>
#include <filter/device/device-filter.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <filter/role/role-filter.hxx>
#include <filter/valid-json/valid-json-filter.hxx>
#include <productivity/productivity-config.hxx>
#include <server/listener-config.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <unistd.h>

#include <chrono>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>

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

// Ruling AM: share/member target validation reads identity.db through the
// named identity client (the same mechanism the gateway uses), so targets
// created after the cutover are shareable. With no [identity] db configured
// the fallback to the default client keeps this boot identity-free.
void installIdentityClient()
{
  const auto path = ConfigService::getString("identity.db");
  if (path.empty())
    return;

  // The gateway creates identity.db at its own boot, which on a fresh
  // install may land after ours; wait bounded before opening it read-only.
  for (int ms = 0; ms < 30000 && !std::filesystem::exists(path); ms += 250)
    std::this_thread::sleep_for(std::chrono::milliseconds(250));

  DbService::enableUriFilenames();
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
  // SQLite URI filenames must be configured before the first sqlite3_open
  // opens the read-only identity database above.
  DbService::enableUriFilenames();

  ConfigService::load("config.toml");

  installIdentityClient();

  const ProductivityDbConfig productivityDb = ProductivityConfig::resolveDb();
  const ListenerConfig listener = ListenerConfig::resolve();

  drogon::app().registerController(std::make_shared<HealthController>());

  drogon::app().registerFilter(std::make_shared<DeviceFilter>());
  drogon::app().registerFilter(std::make_shared<ValidJsonFilter>());
  drogon::app().registerFilter(std::make_shared<JwtFilter>());
  drogon::app().registerFilter(std::make_shared<RoleFilter>());

  drogon::app().loadConfigJson(drogonConfig(productivityDb, listener));

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
           << " (plain); productivity database " << productivityDb.dbPath;

  drogon::app().registerBeginningAdvice([&productivityDb]() {
    if (!DbService::runScriptFile(productivityDb.schemaPath)) {
      LOG_FATAL
          << "Productivity database schema failed to apply — aborting startup";
      _exit(1);
    }

    DbService::applyPragmas();
    // The productivity tables reference user rows that live in identity.db,
    // so foreign-key enforcement stays off on every connection (Ruling AM).
    DbService::client()->execSqlSync("PRAGMA foreign_keys = OFF");
  });

  drogon::app()
      .setThreadNum(0)
      .run();
  return 0;
}