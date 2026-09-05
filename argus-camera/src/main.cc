#include <camera/camera-config.hxx>
#include <config/app-config.hxx>
#include <controllers/health-controller.hxx>
#include <drogon/drogon.h>
#include <server/listener-config.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <unistd.h>

#include <json/value.h>
#include <memory>
#include <stdexcept>
#include <string>

namespace
{

Json::Value drogonConfig(const CameraDbConfig& cameraDb,
                         const ListenerConfig& listener)
{
  Json::Value config = ConfigService::drogonConfig();
  if (config.isNull())
    config = Json::Value(Json::objectValue);

  Json::Value clients(Json::arrayValue);
  Json::Value client(Json::objectValue);
  client["name"] = "default";
  client["rdbms"] = "sqlite3";
  client["filename"] = cameraDb.dbPath;
  client["is_fast"] = false;
  client["number_of_connections"] = 1;
  client["timeout"] = -1.0;
  clients.append(client);
  config["db_clients"] = clients;

  config["listeners"] = listenerJson(listener);

  return config;
}

// Placeholder for the F2-2 cutover: camera routes validating the caller will
// read the gateway-minted identity database through the named identity
// client. With no [identity] db configured the fallback to the default
// client keeps this boot identity-free.
void installIdentityClient()
{
  const auto path = ConfigService::getString("identity.db");
  if (path.empty())
    return;

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

  const CameraDbConfig cameraDb = CameraConfig::resolveDb();
  const ListenerConfig listener = ListenerConfig::resolve();

  drogon::app().registerController(std::make_shared<HealthController>());

  drogon::app().loadConfigJson(drogonConfig(cameraDb, listener));

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
           << " (plain); camera database " << cameraDb.dbPath;

  drogon::app().registerBeginningAdvice([&cameraDb]() {
    DbService::installExtensions();

    if (!DbService::runScriptFile(cameraDb.schemaPath)) {
      LOG_FATAL << "Camera database schema failed to apply — aborting startup";
      _exit(1);
    }

    DbService::applyPragmas();
  });

  drogon::app()
      .setThreadNum(0)
      .run();
  return 0;
}
