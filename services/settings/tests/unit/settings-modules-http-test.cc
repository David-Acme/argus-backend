#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <app/rpc/modules-rpc-service.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/request-context.hxx>
#include <auth/role-access.hxx>
#include <drogon/drogon.h>
#include <errors/response-exception.hxx>
#include <feature/modules/controllers/modules-controller.hxx>
#include <feature/modules/infra/module-catalog-file.hxx>
#include <grpcpp/grpcpp.h>
#include <json/reader.h>
#include <settings/settings-client.hxx>
#include <sqlite/db-service.hxx>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>

namespace
{
namespace fs = std::filesystem;

class NoOwners final : public ComponentOwners
{
public:
  [[nodiscard]] OwnerStates states(const std::string&, const std::vector<ComponentSpec>&) const override
  {
    return {.reach = OwnerReach::Unsupported, .states = {}};
  }
  [[nodiscard]] OwnerReply<ComponentStatus> install(const std::string&, const ComponentSpec&) const override { return {}; }
  [[nodiscard]] OwnerReply<ComponentStatus> cancel(const std::string&, const ComponentSpec&) const override { return {}; }
  [[nodiscard]] OwnerReply<ComponentStatus> remove(const std::string&, const ComponentSpec&) const override { return {}; }
  [[nodiscard]] OwnerReply<ModuleDataSummary> dataSummary(const std::string&, const std::string&) const override
  {
    return {.reach = OwnerReach::Unsupported, .value = std::nullopt};
  }
  [[nodiscard]] OwnerReply<ModuleDataPurge> purgeData(const std::string&, const std::string&) const override { return {}; }
  [[nodiscard]] OwnerReply<PinVerdict> verifyPin(const std::string&, const OwnerPinCheck&) const override
  {
    return {.reach = OwnerReach::Unsupported, .value = std::nullopt};
  }
};

ModuleCatalog shippedCatalog()
{
  auto catalog = loadModuleCatalog(ARGUS_SETTINGS_MODULES_FILE);
  CHECK(catalog.has_value());
  return std::move(catalog).value_or(ModuleCatalog{});
}

struct Service
{
  fs::path path;
  drogon::orm::DbClientPtr db;
  NoOwners owners;
  std::unique_ptr<ModuleEngine> engine;
  std::unique_ptr<ModulesController> controller;

  Service()
      : path(fs::temp_directory_path() /
             ("argus-modules-http-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
              ".db"))
  {
    db = drogon::orm::DbClient::newSqlite3Client("filename=" + path.string(), 1);
    REQUIRE(DbService::runScriptFile(ARGUS_SETTINGS_SCHEMA_FILE, db));
    engine = std::make_unique<ModuleEngine>(ModuleEngineInput{
        .catalog = shippedCatalog(),
        .db = db,
        .owners = owners,
        .events = nullptr,
        .host = [] {
          return HostResources{.ramTotalMb = 32000, .freeDiskBytes = std::nullopt, .cpuFeatures = {"avx2"}, .gpu = false};
        },
        .clock = [] { return std::int64_t{1000000}; },
        .timing = {}});
    engine->tick();
    controller = std::make_unique<ModulesController>(engine.get());
  }

  ~Service()
  {
    engine.reset();
    db.reset();
    std::error_code error;
    fs::remove(path, error);
  }

  Service(const Service&) = delete;
  Service& operator=(const Service&) = delete;
};

drogon::HttpRequestPtr requestAs(UserRole role, const std::string& language)
{
  auto req = drogon::HttpRequest::newHttpRequest();
  req->attributes()->insert(AuthContext::kJwtKey, JwtContext{.sub = 5,
                                                            .name = "test",
                                                            .role = role,
                                                            .isActive = true,
                                                            .deviceHash = "device",
                                                            .sessionId = "session"});
  if (!language.empty())
    req->addHeader("accept-language", language);
  return req;
}

Json::Value bodyOf(const drogon::HttpResponsePtr& response)
{
  Json::CharReaderBuilder builder;
  Json::Value root;
  std::string errors;
  std::istringstream stream(std::string(response->body()));
  REQUIRE(Json::parseFromStream(builder, stream, &root, &errors));
  return root;
}
}

TEST_CASE("GET /modules gives the owner the full catalog and every other role the short list")
{
  Service service;
  const auto owner = drogon::sync_wait(service.controller->list(requestAs(UserRole::Owner, "en-US,en;q=0.9")));
  CHECK(owner->getStatusCode() == drogon::k200OK);
  const auto modules = bodyOf(owner)["info"]["modules"];
  REQUIRE(modules.size() == 4);
  CHECK(modules[0]["id"] == "core");
  CHECK(modules[0]["name"] == "Assistant and home");
  CHECK(modules[0]["lifecycle"] == "active");
  CHECK(modules[1]["id"] == "surveillance");
  CHECK(modules[1]["kind"] == "available");
  CHECK(modules[1]["sizeBytes"].asInt64() == 578108270);
  CHECK(modules[1]["components"].size() == 2);
  CHECK(modules[1]["components"][0]["hostCommand"] == "services/camera/scripts/provision.sh");
  CHECK(modules[1]["components"][1]["hostCommand"].isNull());
  CHECK(modules[1].isMember("hardware"));
  CHECK(modules[1].isMember("gettingStarted"));
  CHECK(modules[3]["kind"] == "coming_soon");

  for (const auto role : {UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
    const auto member = drogon::sync_wait(service.controller->list(requestAs(role, "")));
    const auto list = bodyOf(member)["info"]["modules"];
    REQUIRE(list.size() == 4);
    CHECK(list[1]["name"] == "Vigilancia");
    CHECK(list[1].getMemberNames() ==
          std::vector<std::string>{"dataPurgedAt", "enabled", "id", "lifecycle", "name"});
  }
}

TEST_CASE("the module routes answer 503 when the catalog or the database could not be loaded")
{
  ModulesController unavailable(nullptr);
  int status = 0;
  try {
    static_cast<void>(drogon::sync_wait(unavailable.list(requestAs(UserRole::Owner, ""))));
  }
  catch (const ResponseException& error) {
    status = error.statusCode();
  }
  CHECK(status == 503);
}

TEST_CASE("only the owner reaches the module actions; every role reads the list")
{
  for (const auto role : {UserRole::Owner, UserRole::Resident, UserRole::Guard, UserRole::Guest})
    CHECK(role_access::hasHttpAccess({.role = role, .path = "/modules", .method = drogon::Get}));
  for (const auto role : {UserRole::Resident, UserRole::Guard, UserRole::Guest})
    for (const auto* path : {"/modules/surveillance/install", "/modules/surveillance/pause",
                             "/modules/surveillance/resume", "/modules/surveillance/cancel",
                             "/modules/surveillance/disable", "/modules/surveillance/uninstall"}) {
      INFO(path);
      CHECK_FALSE(role_access::hasHttpAccess({.role = role, .path = path, .method = drogon::Post}));
    }
  for (const auto role : {UserRole::Resident, UserRole::Guard, UserRole::Guest})
    CHECK_FALSE(role_access::hasHttpAccess({.role = role, .path = "/modules/surveillance/data", .method = drogon::Get}));
  CHECK(role_access::hasHttpAccess({.role = UserRole::Owner, .path = "/modules/surveillance/uninstall", .method = drogon::Post}));
}

TEST_CASE("the ModuleStates RPC serves the enabled set to a paired caller and refuses a stranger")
{
  Service service;
  const auto gate = std::make_shared<const argus::client::FleetCallerGate>(argus::client::FleetGateConfig{
      .expectedCallers = {"camera"},
      .callerPairs = {{"camera", "camera-secret"}},
      .legacySecret = {},
      .onFirstLegacy = {}});
  ModulesRpcService rpc([&service] { return service.engine->enabledSet(); }, gate);
  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&rpc);
  const auto server = builder.BuildAndStart();
  const std::string target = "127.0.0.1:" + std::to_string(port);

  const auto reply =
      ModulesClient({.target = target, .credential = "camera-secret", .timeout = std::chrono::seconds(5)}).moduleStates();
  CHECK(reply.settled);
  REQUIRE(reply.modules.size() == 4);
  CHECK(reply.modules[0].id == "core");
  CHECK(reply.modules[0].enabled);
  CHECK(reply.modules[0].lifecycle == "active");
  CHECK(reply.modules[1].enabled);

  int status = 0;
  try {
    static_cast<void>(
        ModulesClient({.target = target, .credential = "wrong", .timeout = std::chrono::seconds(5)}).moduleStates());
  }
  catch (const ResponseException& error) {
    status = error.statusCode();
  }
  CHECK(status == 401);
  server->Shutdown();
}
