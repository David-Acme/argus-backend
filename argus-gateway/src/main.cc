#include <controllers/health-controller.hxx>
#include <drogon/drogon.h>
#include <identity/identity-config.hxx>
#include <identity/identity-registrar.hxx>
#include <config/app-config.hxx>
#include <json/value.h>
#include <memory>
#include <shared/services/cert/cert-service.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/face/face-service.hxx>
#include <shared/services/sqlite/db-service.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>
#include <unistd.h>

#include <cstdint>
#include <string>

namespace
{

Json::Value drogonConfig(const IdentityDbConfig& identityDb)
{
  Json::Value config = ConfigService::drogonConfig();
  if (config.isNull())
    config = Json::Value(Json::objectValue);

  Json::Value clients(Json::arrayValue);
  Json::Value client(Json::objectValue);
  client["name"] = "default";
  client["rdbms"] = "sqlite3";
  client["filename"] = identityDb.dbPath;
  client["is_fast"] = false;
  client["number_of_connections"] = 1;
  client["timeout"] = -1.0;
  clients.append(client);
  config["db_clients"] = clients;
  return config;
}

} // namespace

int main()
{
  ConfigService::load("config.toml");

  const IdentityDbConfig identityDb = IdentityConfig::resolveDb();
  // One database drives the whole identity domain: the Drogon default client
  // and VecDb (face-db) both read the config-driven path below.
  ConfigService::setRuntimeString("database.file", identityDb.dbPath);

  drogon::app().loadConfigJson(drogonConfig(identityDb));

  drogon::app().registerController(std::make_shared<HealthController>());
  const IdentityRegistrationStats identity =
      registerIdentitySurface();
  LOG_INFO << "Identity surface registered: " << identity.controllers
           << " controllers, " << identity.filters << " filters";

  drogon::app().registerPreRoutingAdvice(
      [](const drogon::HttpRequestPtr& req,
         drogon::AdviceCallback&& cb,
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

  const std::string natsUrl = ConfigService::getString("nats.url");
  std::unique_ptr<NatsBus> natsBus;
  if (natsUrl.empty()) {
    LOG_INFO << "NATS not configured; event bus disabled";
  } else {
    natsBus = std::make_unique<NatsBus>();
    if (natsBus->connect())
      LOG_INFO << "NATS event bus connected to " << natsBus->options().url;
    else
      LOG_WARN << "NATS unavailable at " << natsUrl
               << "; continuing without it";
  }

  drogon::app().registerBeginningAdvice([&identityDb = identityDb]() {
    DbService::installExtensions();

    if (!DbService::runScriptFile(identityDb.schemaPath)) {
      LOG_FATAL << "Identity database schema failed to apply — aborting startup";
      _exit(1);
    }

    DbService::applyPragmas();

    if (ConfigService::getBool("face.enabled")) {
      FaceService::instance().init();
      if (!FaceService::instance().isLoaded())
        LOG_WARN << "FaceService not loaded — facial login disabled";
    }
    else {
      LOG_INFO << "FaceService disabled by configuration";
    }

    if (!CertService::init())
      LOG_WARN << "PKI not loaded — pairing disabled";
  });

  const int port = ConfigService::getInt("gateway.port");
  std::string host = ConfigService::getString("gateway.host");
  if (host.empty())
    host = "0.0.0.0";

  drogon::app()
      .addListener(host, static_cast<uint16_t>(port > 0 ? port : 7024))
      .setThreadNum(0)
      .run();
  return 0;
}
