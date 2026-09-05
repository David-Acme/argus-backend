#include <camera/camera-config.hxx>
#include <camera/nats-camera-change-sink.hxx>
#include <config/app-config.hxx>
#include <controllers/camera-media-service.hxx>
#include <controllers/health-controller.hxx>
#include <drogon/drogon.h>
#include <feature/api/camera/controllers/camera-controller.hxx>
#include <feature/api/zone/controllers/zone-controller.hxx>
#include <feature/socket/sync/socket/sync-socket.hxx>
#include <filter/device/device-filter.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <filter/role/role-filter.hxx>
#include <filter/valid-json/valid-json-filter.hxx>
#include <server/listener-config.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/room/room-manager.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/services/stream/go2rtc-manager.hxx>
#include <shared/services/stream/stream-hub.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>
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
  // The camera and zone controllers live in the shared static library, so
  // their AutoCreation registration is linker-dropped there; the legacy
  // registers the same classes explicitly.
  drogon::app().registerController(std::make_shared<CameraController>());
  drogon::app().registerController(std::make_shared<ZoneController>());

  drogon::app().registerFilter(std::make_shared<DeviceFilter>());
  drogon::app().registerFilter(std::make_shared<ValidJsonFilter>());
  drogon::app().registerFilter(std::make_shared<JwtFilter>());
  drogon::app().registerFilter(std::make_shared<RoleFilter>());

  const auto syncSocket = std::make_shared<SyncSocket>();
  syncSocket->setForwarder(std::make_shared<CameraMediaService>());
  drogon::app().registerController(syncSocket);

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

  // The camera-domain change funnel (Ruling Y): without NATS configured the
  // camera feature services drop their change events with a warning, which
  // keeps a NATS-less camera service bootable for contract tests.
  std::shared_ptr<NatsCameraChangeSink> changeSink;
  const std::string natsUrl = ConfigService::getString("nats.url");
  if (natsUrl.empty()) {
    LOG_INFO << "NATS not configured; camera change funnel disabled";
  }
  else {
    auto bus = std::make_shared<NatsBus>();
    if (bus->connect()) {
      LOG_INFO << "NATS event bus connected to " << bus->options().url;
      changeSink = std::make_shared<NatsCameraChangeSink>(std::move(bus));
      camera_change::setSink(changeSink.get());
    }
    else {
      LOG_WARN << "NATS unavailable at " << natsUrl
               << "; camera change funnel disabled";
    }
  }

  RoomManager roomManagerLifecycle;
  roomManagerLifecycle.init();

  drogon::app().registerBeginningAdvice([&cameraDb]() {
    DbService::installExtensions();

    if (!DbService::runScriptFile(cameraDb.schemaPath)) {
      LOG_FATAL << "Camera database schema failed to apply — aborting startup";
      _exit(1);
    }

    DbService::applyPragmas();

    Go2rtcManager::instance().init();
    StreamHub::instance().init();
  });

  drogon::app()
      .setThreadNum(0)
      .run();

  StreamHub::instance().shutdown();
  Go2rtcManager::instance().shutdown();
  roomManagerLifecycle.shutdown();
  return 0;
}
