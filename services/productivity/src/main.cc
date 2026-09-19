#include <config/app-config.hxx>
#include <controllers/health-controller.hxx>
#include <drogon/drogon.h>
#include <feature/sync/productivity-sync-rpc-service.hxx>
#include <filter/device/device-filter.hxx>
#include <filter/jwt/jwt-filter.hxx>
#include <filter/role/role-filter.hxx>
#include <filter/valid-json/valid-json-filter.hxx>
#include <grpcpp/grpcpp.h>
#include <productivity/productivity-config.hxx>
#include <productivity/nats-productivity-change-sink.hxx>
#include <shared/wrapper/nats/nats-bus.hxx>
#include <server/listener-config.hxx>
#include <shared/contracts/user-change-sink.hxx>
#include <shared/services/config-service/config-service.hxx>
#include <shared/services/sqlite/db-service.hxx>
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

} // namespace

int main()
{
  DbService::enableUriFilenames();

  ConfigService::load("config.toml");

  const ProductivityDbConfig productivityDb = ProductivityConfig::resolveDb();
  const ListenerConfig listener = ListenerConfig::resolve(7027);
  const GrpcListenerConfig grpcListener = GrpcListenerConfig::resolve(7037);

  ProductivitySyncRpcService productivitySyncRpc;

  grpc::ServerBuilder grpcBuilder;
  const std::string grpcAddress =
      grpcListener.host + ":" + std::to_string(grpcListener.port);
  grpcBuilder.AddListeningPort(grpcAddress, grpc::InsecureServerCredentials());
  grpcBuilder.RegisterService(&productivitySyncRpc);
  std::unique_ptr<grpc::Server> grpcServer(grpcBuilder.BuildAndStart());
  if (!grpcServer) {
    LOG_FATAL << "gRPC server failed to listen on " << grpcAddress;
    return 1;
  }

  drogon::app().registerController(std::make_shared<HealthController>(HealthStatus{.serviceName = "argus-productivity", .extras = {}}));

  drogon::app().registerFilter(std::make_shared<DeviceFilter>());
  drogon::app().registerFilter(std::make_shared<ValidJsonFilter>());
  drogon::app().registerFilter(std::make_shared<JwtFilter>());
  drogon::app().registerFilter(std::make_shared<RoleFilter>());

  drogon::app().loadConfigJson(drogonConfig(productivityDb, listener));

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
           << " (plain); productivity database " << productivityDb.dbPath
           << "; gRPC SyncService on " << grpcAddress;

  drogon::app().registerBeginningAdvice([&productivityDb]() {
    if (!DbService::runScriptFile(productivityDb.schemaPath)) {
      LOG_FATAL
          << "Productivity database schema failed to apply — aborting startup";
      _exit(1);
    }

    DbService::applyPragmas();
    DbService::client()->execSqlSync("PRAGMA foreign_keys = OFF");
  });

  std::shared_ptr<NatsProductivityChangeSink> changeSink;
  std::shared_ptr<NatsBus> natsBus;
  const std::string natsUrl = ConfigService::getString("nats.url");
  if (natsUrl.empty()) {
    LOG_INFO << "NATS not configured; productivity change funnel disabled";
  }
  else {
    natsBus = std::make_shared<NatsBus>();
    if (natsBus->connect()) {
      LOG_INFO << "NATS event bus connected to " << natsBus->options().url;
      changeSink = std::make_shared<NatsProductivityChangeSink>(natsBus);
      user_change::setProductivitySink(changeSink.get());
    }
    else {
      natsBus.reset();
      LOG_WARN << "NATS unavailable at " << natsUrl
               << "; productivity change funnel disabled";
    }
  }

  drogon::app()
      .setThreadNum(0)
      .run();

  grpcServer->Shutdown();
  return 0;
}
