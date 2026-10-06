#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/orm/DbClient.h>
#include <feature/mcp/infra/engine-module-desk.hxx>
#include <feature/modules/infra/module-catalog-file.hxx>
#include <sqlite/db-service.hxx>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace
{
namespace fs = std::filesystem;

class QuietOwners final : public ComponentOwners
{
public:
  [[nodiscard]] OwnerStates states(const std::string&, const std::vector<ComponentSpec>& specs) const override
  {
    OwnerStates states{.reach = OwnerReach::Answered, .states = {}};
    for (const auto& spec : specs)
      states.states.push_back({.id = spec.id,
                               .state = ComponentState::Missing,
                               .bytesPresent = 0,
                               .bytesTotal = spec.totalBytes(),
                               .ready = false,
                               .hostCommand = {},
                               .reason = {}});
    return states;
  }

  [[nodiscard]] OwnerReply<ComponentStatus> install(const std::string&, const ComponentSpec& spec) const override
  {
    return {.reach = OwnerReach::Answered,
            .value = ComponentStatus{.id = spec.id,
                                     .state = ComponentState::Installing,
                                     .bytesPresent = 0,
                                     .bytesTotal = spec.totalBytes(),
                                     .ready = false,
                                     .hostCommand = {},
                                     .reason = {}}};
  }

  [[nodiscard]] OwnerReply<ComponentStatus> cancel(const std::string&, const ComponentSpec&) const override
  {
    return {.reach = OwnerReach::Answered, .value = std::nullopt};
  }

  [[nodiscard]] OwnerReply<ComponentStatus> remove(const std::string&, const ComponentSpec&) const override
  {
    return {.reach = OwnerReach::Answered, .value = std::nullopt};
  }

  [[nodiscard]] OwnerReply<ModuleDataSummary> dataSummary(const std::string&, const std::string&) const override
  {
    return {.reach = OwnerReach::Answered, .value = ModuleDataSummary{}};
  }

  [[nodiscard]] OwnerReply<ModuleDataPurge> purgeData(const std::string&, const std::string&) const override
  {
    return {.reach = OwnerReach::Answered, .value = ModuleDataPurge{.purged = true, .reason = {}}};
  }

  [[nodiscard]] OwnerReply<PinVerdict> verifyPin(const std::string&, const OwnerPinCheck&) const override
  {
    return {.reach = OwnerReach::Unsupported, .value = std::nullopt};
  }
};

struct Named
{
  std::string id;
  std::string kind;
  std::vector<std::string> required;
};

Json::Value named(const Named& input)
{
  const auto& [id, kind, required] = input;
  Json::Value json(Json::objectValue);
  json["id"] = id;
  json["kind"] = kind;
  json["name"]["es"] = "Nombre " + id;
  json["name"]["en"] = "Name " + id;
  json["summary"]["es"] = "Resumen " + id;
  json["summary"]["en"] = "Summary " + id;
  json["requires"] = Json::Value(Json::arrayValue);
  for (const auto& entry : required)
    json["requires"].append(entry);
  json["components"] = Json::Value(Json::arrayValue);
  json["gates"] = Json::Value(Json::arrayValue);
  json["dataOwners"] = Json::Value(Json::arrayValue);
  json["hardware"]["minRamMb"] = 1000;
  json["hardware"]["recommendedRamMb"] = 2000;
  json["gettingStarted"] = Json::Value(Json::arrayValue);
  return json;
}

ModuleCatalog catalogOfDesk()
{
  Json::Value root(Json::objectValue);
  root["components"] = Json::Value(Json::arrayValue);
  Json::Value file(Json::objectValue);
  file["path"] = "vision/model.bin";
  file["sizeBytes"] = 1000;
  file["url"] = "https://example.test/1abed04b6fe71314d8c446a1371c03d7c332266d/model.bin";
  file["sha256"] = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
  Json::Value vision(Json::objectValue);
  vision["id"] = "vision";
  vision["owner"] = "vlm";
  vision["source"] = "download";
  vision["ramMb"] = 100;
  vision["files"].append(file);
  root["components"].append(vision);
  root["modules"].append(named({.id = "core", .kind = "core", .required = {}}));
  auto productivity = named({.id = "productivity", .kind = "available", .required = {"core"}});
  productivity["intro"]["es"]["what"] = "Organiza tu agenda.";
  productivity["intro"]["es"]["examples"].append("agenda una reunión");
  productivity["intro"]["en"]["what"] = "Organizes your agenda.";
  productivity["intro"]["en"]["examples"].append("schedule a meeting");
  root["modules"].append(productivity);
  root["modules"].append(named({.id = "insights", .kind = "available", .required = {"productivity"}}));
  auto surveillance = named({.id = "surveillance", .kind = "available", .required = {"core"}});
  surveillance["components"].append("vision");
  root["modules"].append(surveillance);
  root["modules"].append(named({.id = "agronomy", .kind = "coming_soon", .required = {"core"}}));
  auto parsed = parseModuleCatalog(root);
  INFO(parsed.problem);
  REQUIRE(parsed.catalog.has_value());
  return std::move(parsed.catalog).value_or(ModuleCatalog{});
}

struct Harness
{
  fs::path path;
  drogon::orm::DbClientPtr db;
  QuietOwners owners;
  std::atomic<std::int64_t> now{1000000};
  HostResources host{.ramTotalMb = 16000, .freeDiskBytes = 1099511627776, .cpuFeatures = {"avx2"}, .gpu = false};
  std::unique_ptr<ModuleEngine> engine;
  std::shared_ptr<EngineModuleDesk> desk;

  Harness()
      : path(fs::temp_directory_path() /
             ("argus-engine-desk-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".db"))
  {
    db = drogon::orm::DbClient::newSqlite3Client("filename=" + path.string(), 1);
    REQUIRE(DbService::runScriptFile(ARGUS_SETTINGS_SCHEMA_FILE, db));
    engine = std::make_unique<ModuleEngine>(ModuleEngineInput{
        .catalog = catalogOfDesk(),
        .db = db,
        .owners = owners,
        .events = nullptr,
        .host = [this] { return host; },
        .clock = [this] { return now.load(); },
        .timing = {.catalogPath = {},
                   .dbPath = {},
                   .schemaPath = {},
                   .modelsDir = {},
                   .pollInterval = std::chrono::milliseconds(1000),
                   .idleRefresh = std::chrono::seconds(30),
                   .healthTimeout = std::chrono::seconds(60),
                   .seedWait = std::chrono::seconds(300)}});
    desk = std::make_shared<EngineModuleDesk>(EngineModuleDeskInput{.engine = engine.get()});
  }

  ~Harness()
  {
    desk.reset();
    engine.reset();
    db.reset();
    std::error_code error;
    fs::remove(path, error);
    fs::remove(path.string() + "-wal", error);
    fs::remove(path.string() + "-shm", error);
  }

  Harness(const Harness&) = delete;
  Harness& operator=(const Harness&) = delete;

  void settle()
  {
    now += 1000;
    engine->tick();
  }

  [[nodiscard]] ModuleCard card(const std::string& id, const std::string& lang = "es") const
  {
    auto found = drogon::sync_wait(desk->find({.moduleId = id, .lang = lang}));
    REQUIRE(found.has_value());
    return std::move(found).value_or(ModuleCard{});
  }

  [[nodiscard]] EnableOutcome enable(const std::string& id, const std::string& lang = "es") const
  {
    return drogon::sync_wait(desk->enable({.moduleId = id, .userId = 1, .lang = lang}));
  }

  [[nodiscard]] DisableOutcome disable(const std::string& id, const std::string& lang = "es") const
  {
    return drogon::sync_wait(desk->disable({.moduleId = id, .userId = 1, .lang = lang}));
  }
};
}

TEST_CASE("the desk lists every module in the user's language with its state and intro")
{
  Harness harness;
  harness.settle();
  const auto cards = drogon::sync_wait(harness.desk->list("es"));
  REQUIRE(cards.size() == 5);
  CHECK(harness.card("core").state == ModuleState::Active);
  CHECK(harness.card("productivity").state == ModuleState::Active);
  CHECK(harness.card("surveillance").state == ModuleState::Off);
  CHECK(harness.card("agronomy").state == ModuleState::ComingSoon);

  const auto spanish = harness.card("productivity", "es");
  CHECK(spanish.name == "Nombre productivity");
  CHECK(spanish.summary == "Resumen productivity");
  CHECK(spanish.what == "Organiza tu agenda.");
  CHECK(spanish.examples == std::vector<std::string>{"agenda una reunión"});
  const auto english = harness.card("productivity", "en");
  CHECK(english.name == "Name productivity");
  CHECK(english.what == "Organizes your agenda.");
  CHECK(english.examples == std::vector<std::string>{"schedule a meeting"});

  CHECK_FALSE(drogon::sync_wait(harness.desk->find({.moduleId = "nope", .lang = "es"})).has_value());
}

TEST_CASE("enabling starts the install and the card then says it is installing")
{
  Harness harness;
  harness.settle();
  const auto started = harness.enable("surveillance");
  CHECK(started.kind == EnableKind::Started);
  CHECK(harness.card("surveillance").state == ModuleState::Installing);
  CHECK(harness.enable("surveillance").kind == EnableKind::JobRunning);
}

TEST_CASE("enabling answers the engine's refusals as spoken kinds")
{
  Harness harness;
  CHECK(harness.enable("surveillance").kind == EnableKind::Unavailable);
  harness.settle();
  CHECK(harness.enable("productivity").kind == EnableKind::AlreadyActive);
  CHECK(harness.enable("agronomy").kind == EnableKind::ComingSoon);
  CHECK(harness.enable("nope").kind == EnableKind::Unknown);
  harness.host.ramTotalMb = 100;
  CHECK(harness.enable("surveillance").kind == EnableKind::HardwareInsufficient);
}

TEST_CASE("disabling turns a module off and refuses the core and the ones another module needs, in the user's language")
{
  Harness harness;
  harness.settle();

  const auto base = harness.disable("core");
  CHECK(base.kind == DisableKind::Refused);
  CHECK(base.detail == "el módulo base siempre está activo");
  CHECK(harness.disable("core", "en").detail == "the core module is always on");

  const auto needed = harness.disable("productivity");
  CHECK(needed.kind == DisableKind::Refused);
  CHECK(needed.detail == "otro módulo que está activo lo necesita");
  CHECK(harness.disable("productivity", "en").detail == "another module that is on needs it");
  CHECK(harness.card("productivity").state == ModuleState::Active);

  CHECK(harness.disable("insights").kind == DisableKind::Disabled);
  CHECK(harness.card("insights").state == ModuleState::Off);
  CHECK(harness.disable("productivity").kind == DisableKind::Disabled);
  CHECK(harness.card("productivity").state == ModuleState::Off);
  CHECK(harness.disable("nope").kind == DisableKind::Unknown);
}

TEST_CASE("the impact is never invented: a known module says it cannot be previewed here and an unknown one is unknown")
{
  Harness harness;
  harness.settle();
  const auto spanish = drogon::sync_wait(harness.desk->impact({.moduleId = "productivity", .lang = "es"}));
  CHECK(spanish.known);
  CHECK_FALSE(spanish.allowed);
  CHECK(spanish.refusalCode == "impact_unavailable");
  CHECK(spanish.stops.empty());
  CHECK(spanish.refusal.find("apágalo desde la app") != std::string::npos);
  const auto english = drogon::sync_wait(harness.desk->impact({.moduleId = "productivity", .lang = "en"}));
  CHECK(english.refusal.find("turn it off from the app") != std::string::npos);
  CHECK_FALSE(drogon::sync_wait(harness.desk->impact({.moduleId = "nope", .lang = "es"})).known);
}

TEST_CASE("a request to the owner is unavailable until the request route exists")
{
  Harness harness;
  harness.settle();
  CHECK(drogon::sync_wait(harness.desk->request({.moduleId = "surveillance", .userId = 7, .lang = "es"})).kind ==
        RequestKind::Unavailable);
}

TEST_CASE("without an engine the desk lists nothing and refuses everything")
{
  EngineModuleDesk desk(EngineModuleDeskInput{.engine = nullptr});
  CHECK(drogon::sync_wait(desk.list("es")).empty());
  CHECK_FALSE(drogon::sync_wait(desk.find({.moduleId = "core", .lang = "es"})).has_value());
  CHECK(drogon::sync_wait(desk.enable({.moduleId = "core", .userId = 1, .lang = "es"})).kind == EnableKind::Unavailable);
  CHECK(drogon::sync_wait(desk.disable({.moduleId = "core", .userId = 1, .lang = "es"})).kind == DisableKind::Unavailable);
  CHECK_FALSE(drogon::sync_wait(desk.impact({.moduleId = "core", .lang = "es"})).known);
}
