#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <app/rpc/modules-rpc-service.hxx>
#include <auth/jwt-filter.hxx>
#include <auth/module-gate.hxx>
#include <errors/validation-exception.hxx>
#include <feature/modules/dtos/module-json.hxx>
#include <feature/modules/dtos/uninstall-module-dto.hxx>
#include <feature/modules/infra/module-event-sink.hxx>
#include <text/json-util.hxx>
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
  [[nodiscard]] OwnerReply<ModuleRequestOutcome> requestModule(const std::string&,
                                                               const ModuleRequestInput&) const override
  {
    return requestReply;
  }

  OwnerReply<ModuleRequestOutcome> requestReply{.reach = OwnerReach::Unsupported, .value = std::nullopt};
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

  CHECK(role_access::hasHttpAccess({.role = UserRole::Owner, .path = "/modules/surveillance/impact", .method = drogon::Get}));
  for (const auto role : {UserRole::Resident, UserRole::Guard, UserRole::Guest}) {
    CHECK_FALSE(role_access::hasHttpAccess({.role = role, .path = "/modules/surveillance/impact", .method = drogon::Get}));
    CHECK(role_access::hasHttpAccess({.role = role, .path = "/modules/surveillance/request", .method = drogon::Post}));
  }
  CHECK(role_access::hasHttpAccess({.role = UserRole::Owner, .path = "/modules/surveillance/request", .method = drogon::Post}));
}

TEST_CASE("the ModuleStates RPC serves the enabled set to a paired caller and refuses a stranger")
{
  Service service;
  const auto gate = std::make_shared<const argus::client::FleetCallerGate>(argus::client::FleetGateConfig{
      .expectedCallers = {"camera"},
      .callerPairs = {{"camera", "camera-secret"}},
      .legacySecret = {},
      .onFirstLegacy = {}});
  ModulesRpcService rpc({.states = [&service] { return service.engine->enabledSet(); },
                         .catalog = [&service] {
                           Json::Value list(Json::arrayValue);
                           for (const auto& view : service.engine->list())
                             list.append(module_json::module(view, "es"));
                           return OwnerCatalogReply{.modulesJson = json_util::toString(list),
                                                    .version = service.engine->enabledSet().version};
                         },
                         .gate = gate});
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
  CHECK(reply.modules[0].roles.empty());
  CHECK(reply.modules[1].id == "surveillance");
  CHECK(reply.modules[1].roles == std::vector<std::string>{"guard"});
  CHECK(reply.modules[0].kind == "core");
  CHECK(reply.modules[1].kind == "available");
  CHECK(reply.modules[3].kind == "coming_soon");
  CHECK(reply.modules[1].name.es == "Vigilancia");
  CHECK(reply.modules[1].name.en == "Surveillance");
  CHECK(reply.modules[1].summary.en.starts_with("Live cameras"));
  CHECK_FALSE(reply.modules[1].intro.es.what.empty());
  CHECK(reply.modules[1].intro.es.examples.size() == 3);
  CHECK(reply.modules[1].intro.en.examples.size() == 3);
  CHECK(reply.modules[2].roles.empty());

  const auto catalog =
      ModulesClient({.target = target, .credential = "camera-secret", .timeout = std::chrono::seconds(5)}).ownerCatalog();
  CHECK(catalog.version == reply.version);
  const auto owner = json_util::fromString(catalog.modulesJson);
  REQUIRE(owner.isArray());
  REQUIRE(owner.size() == 4);
  CHECK(owner[1]["id"] == "surveillance");
  CHECK(owner[1]["roles"][0] == "guard");
  CHECK(owner[1]["intro"]["examples"].size() == 3);
  CHECK(owner[1].isMember("hardware"));
  CHECK(owner[1].isMember("components"));

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

TEST_CASE("the enabled-set event is read back by every service's gate with its roles and texts")
{
  Service service;
  const auto set = service.engine->enabledSet();
  const auto flags = module_gate::parseEnabledSet(module_event::enabledPayload(set, 1));
  REQUIRE(flags.has_value());
  const auto modules = flags.value_or(ModuleFlags{});
  REQUIRE(modules.size() == 4);
  CHECK(modules[1].id == "surveillance");
  CHECK(modules[1].roles == std::vector<std::string>{"guard"});
  CHECK(modules[1].kind == "available");
  CHECK(modules[3].kind == "coming_soon");
  CHECK(modules[1].name.es == "Vigilancia");
  CHECK(modules[1].intro.en.examples.size() == 3);
  CHECK(modules[0].roles.empty());

  ModuleGate gate;
  gate.apply(modules);
  CHECK(gate.roleActive(UserRole::Guard) == modules[1].enabled);
  ModuleFlags off = modules;
  off[1].enabled = false;
  gate.apply(off);
  CHECK_FALSE(gate.roleActive(UserRole::Guard));
  CHECK(gate.roleActive(UserRole::Resident));
}

namespace
{
drogon::HttpRequestPtr impactRequest(const std::string& action)
{
  auto req = requestAs(UserRole::Owner, "");
  if (!action.empty())
    req->setParameter("action", action);
  return req;
}

Json::Value parsed(const std::string& text)
{
  Json::CharReaderBuilder builder;
  Json::Value root;
  std::string errors;
  std::istringstream stream(text);
  REQUIRE(Json::parseFromStream(builder, stream, &root, &errors));
  return root;
}
}

TEST_CASE("GET /modules/{id}/impact answers the shape the app renders, with what keeps running from the catalog")
{
  Service service;
  const auto disable = drogon::sync_wait(service.controller->impact(impactRequest("disable"), "surveillance"));
  CHECK(disable->getStatusCode() == drogon::k200OK);
  const auto info = bodyOf(disable)["info"];
  CHECK(info["moduleId"] == "surveillance");
  CHECK(info["action"] == "disable");
  CHECK(info["allowed"].asBool());
  CHECK(info["refusal"].isNull());
  REQUIRE(info["stops"].size() == 8);
  CHECK(info["stops"][0]["kind"] == "live_views");
  CHECK(info["stops"][0]["count"].isNull());
  CHECK(info["roleHolders"].isArray());
  CHECK(info["roleHolders"].empty());
  CHECK(info["roleEffect"] == "none");
  CHECK(info["reassignRoles"].empty());
  CHECK(info["invitations"].isArray());
  CHECK(info["data"]["owners"].isArray());
  CHECK(info["filesBytes"].isInt64());
  REQUIRE(info["keepsRunning"].size() == 1);
  CHECK(info["keepsRunning"][0]["id"] == "safety_alerts");
  CHECK(info["keepsRunning"][0]["text"]["es"] ==
        "Las alertas de pánico o coacción en curso seguirán hasta que alguien las atienda.");
  CHECK(info["keepsRunning"][0]["text"]["en"] ==
        "Panic or duress alerts already raised keep going until someone attends them.");
  CHECK(info["unreachable"].isArray());

  const auto uninstall = bodyOf(drogon::sync_wait(service.controller->impact(impactRequest("uninstall"), "surveillance")))["info"];
  CHECK(uninstall["action"] == "uninstall");
  REQUIRE(uninstall["reassignRoles"].size() == 2);
  CHECK(uninstall["reassignRoles"][0] == "resident");
  CHECK(uninstall["reassignRoles"][1] == "guest");
  CHECK(uninstall["keepsRunning"].size() == 1);

  const auto core = bodyOf(drogon::sync_wait(service.controller->impact(impactRequest("disable"), "core")))["info"];
  CHECK_FALSE(core["allowed"].asBool());
  CHECK(core["refusal"]["code"] == "MODULE_CORE");
  CHECK(core["keepsRunning"].empty());

  const auto productivity = bodyOf(drogon::sync_wait(service.controller->impact(impactRequest("disable"), "productivity")))["info"];
  REQUIRE(productivity["stops"].size() == 1);
  CHECK(productivity["stops"][0]["kind"] == "agenda_calls");
  CHECK(productivity["keepsRunning"].empty());
}

TEST_CASE("the impact route refuses a missing or unknown action with 422 and an unknown module with 404")
{
  Service service;
  CHECK_THROWS_AS(static_cast<void>(drogon::sync_wait(service.controller->impact(impactRequest(""), "surveillance"))),
                  ValidationException);
  CHECK_THROWS_AS(static_cast<void>(drogon::sync_wait(service.controller->impact(impactRequest("purge"), "surveillance"))),
                  ValidationException);
  int status = 0;
  try {
    static_cast<void>(drogon::sync_wait(service.controller->impact(impactRequest("disable"), "ghost")));
  }
  catch (const ResponseException& error) {
    status = error.statusCode();
  }
  CHECK(status == 404);
}

TEST_CASE("POST /modules/{id}/request answers requested with the duplicate flag the owner of notifications reported")
{
  Service service;
  static_cast<void>(service.engine->disable({.moduleId = "productivity", .userId = 1}));
  const auto codeOf = [&](UserRole role, const std::string& id) {
    try {
      static_cast<void>(drogon::sync_wait(service.controller->requestModule(requestAs(role, ""), id)));
    }
    catch (const ResponseException& error) {
      return error.errorCode();
    }
    return std::string();
  };

  CHECK(codeOf(UserRole::Guard, "productivity") == "SERVICE_UNAVAILABLE");
  service.owners.requestReply = {.reach = OwnerReach::Answered,
                                 .value = ModuleRequestOutcome{.notified = 1, .duplicate = false}};
  const auto first = drogon::sync_wait(service.controller->requestModule(requestAs(UserRole::Guard, ""), "productivity"));
  CHECK(first->getStatusCode() == drogon::k200OK);
  const auto info = bodyOf(first)["info"];
  CHECK(info["moduleId"] == "productivity");
  CHECK(info["requested"].asBool());
  CHECK_FALSE(info["duplicate"].asBool());

  service.owners.requestReply = {.reach = OwnerReach::Answered,
                                 .value = ModuleRequestOutcome{.notified = 0, .duplicate = true}};
  CHECK(bodyOf(drogon::sync_wait(service.controller->requestModule(requestAs(UserRole::Resident, ""), "productivity")))["info"]
            ["duplicate"]
                .asBool());

  CHECK(codeOf(UserRole::Owner, "productivity") == "CONFLICT");
  CHECK(codeOf(UserRole::Guard, "core") == "CONFLICT");
  CHECK(codeOf(UserRole::Guard, "agronomy") == "MODULE_COMING_SOON");
  CHECK(codeOf(UserRole::Guard, "ghost") == "NOT_FOUND");
}

TEST_CASE("the uninstall body names a new role for each holder, never the owner role, and refuses what it cannot read")
{
  const auto dto = UninstallModuleDto::fromJson(parsed(R"({"keepData":true,"reassign":{"7":"resident","8":"guest"}})"));
  REQUIRE(dto.reassign.size() == 2);
  CHECK(dto.reassign[0].userId == 7);
  CHECK(dto.reassign[0].role == "resident");
  CHECK(UninstallModuleDto::fromJson(parsed(R"({"keepData":true})")).reassign.empty());
  CHECK(UninstallModuleDto::fromJson(parsed(R"({"keepData":true,"reassign":null})")).reassign.empty());

  for (const auto* body : {R"({"reassign":{"7":"owner"}})", R"({"reassign":{"seven":"guest"}})",
                           R"({"reassign":{"7":"astronaut"}})", R"({"reassign":{"0":"guest"}})",
                           R"({"reassign":{"-3":"guest"}})", R"({"reassign":["guest"]})",
                           R"({"reassign":{"7":4}})", R"({"reassign":"guest"})"}) {
    INFO(body);
    CHECK_THROWS_AS(UninstallModuleDto::fromJson(parsed(body)), ValidationException);
  }
}
