#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <errors/validation-exception.hxx>
#include <feature/modules/dtos/module-json.hxx>
#include <feature/modules/dtos/response-list-modules-dto.hxx>
#include <feature/modules/dtos/uninstall-module-dto.hxx>
#include <feature/modules/infra/module-catalog-file.hxx>
#include <feature/modules/infra/module-event-sink.hxx>
#include <feature/modules/services/hardware-check.hxx>
#include <feature/modules/services/module-event-throttle.hxx>
#include <feature/modules/services/module-resolver.hxx>
#include <json/reader.h>

#include <fstream>
#include <set>
#include <string>

namespace
{
Json::Value parse(const std::string& text)
{
  Json::CharReaderBuilder builder;
  Json::Value root;
  std::string errors;
  std::istringstream stream(text);
  REQUIRE(Json::parseFromStream(builder, stream, &root, &errors));
  return root;
}

Json::Value shipped()
{
  std::ifstream file(ARGUS_SETTINGS_MODULES_FILE);
  Json::CharReaderBuilder builder;
  Json::Value root;
  std::string errors;
  REQUIRE(Json::parseFromStream(builder, file, &root, &errors));
  return root;
}

Json::Value noHardware()
{
  Json::Value json(Json::objectValue);
  json["minRamMb"] = 0;
  json["recommendedRamMb"] = 0;
  json["requiredCpu"] = Json::Value(Json::arrayValue);
  json["recommendedCpu"] = Json::Value(Json::arrayValue);
  return json;
}

struct ModuleInput
{
  std::string id;
  std::string kind;
  std::vector<std::string> required;
};

Json::Value module(const ModuleInput& input)
{
  const auto& [id, kind, required] = input;
  Json::Value json(Json::objectValue);
  json["id"] = id;
  json["kind"] = kind;
  json["name"]["es"] = id;
  json["name"]["en"] = id;
  json["summary"]["es"] = id;
  json["summary"]["en"] = id;
  json["requires"] = Json::Value(Json::arrayValue);
  for (const auto& entry : required)
    json["requires"].append(entry);
  json["components"] = Json::Value(Json::arrayValue);
  json["gates"] = Json::Value(Json::arrayValue);
  json["dataOwners"] = Json::Value(Json::arrayValue);
  json["hardware"] = noHardware();
  json["gettingStarted"] = Json::Value(Json::arrayValue);
  return json;
}

Json::Value catalogOf(const std::vector<Json::Value>& modules)
{
  Json::Value root(Json::objectValue);
  root["components"] = Json::Value(Json::arrayValue);
  root["modules"] = Json::Value(Json::arrayValue);
  for (const auto& entry : modules)
    root["modules"].append(entry);
  return root;
}

std::string problemOf(const Json::Value& root)
{
  const auto parsed = parseModuleCatalog(root);
  CHECK_FALSE(parsed.catalog.has_value());
  return parsed.problem;
}

ModuleCatalog testCatalog()
{
  auto parsed = parseModuleCatalog(catalogOf({module({.id = "core", .kind = "core", .required = {}}), module({.id = "a", .kind = "available", .required = {"core"}}),
                                              module({.id = "b", .kind = "available", .required = {"a"}})}));
  CHECK(parsed.catalog.has_value());
  auto catalog = std::move(parsed.catalog).value_or(ModuleCatalog{});
  catalog.modules[0].hardware = {.minRamMb = 4000, .recommendedRamMb = 8000, .requiredCpu = {}, .recommendedCpu = {}, .recommendedGpu = false};
  catalog.modules[1].hardware = {.minRamMb = 1000, .recommendedRamMb = 2000, .requiredCpu = {}, .recommendedCpu = {"avx2"}, .recommendedGpu = false};
  catalog.modules[2].hardware = {.minRamMb = 500, .recommendedRamMb = 500, .requiredCpu = {"neon"}, .recommendedCpu = {}, .recommendedGpu = true};
  return catalog;
}
}

TEST_CASE("the shipped catalog is valid and names its modules, components and data owners")
{
  const auto parsed = parseModuleCatalog(shipped());
  INFO(parsed.problem);
  if (!parsed.catalog) {
    FAIL("the shipped catalog is invalid");
    return;
  }
  const auto& catalog = *parsed.catalog;
  REQUIRE(catalog.module("core") != nullptr);
  CHECK(catalog.module("core")->kind == ModuleKind::Core);
  CHECK(catalog.module("surveillance")->kind == ModuleKind::Available);
  CHECK(catalog.module("productivity")->components.empty());
  CHECK(catalog.module("agronomy")->kind == ModuleKind::ComingSoon);
  CHECK(catalog.component("vision")->spec.source == ComponentSource::Download);
  CHECK(catalog.component("vision")->owner == "vlm");
  CHECK(catalog.component("detector")->spec.source == ComponentSource::Provisioned);
  CHECK(catalog.component("llm")->owner == "llm");
  CHECK(catalog.sizeBytes(*catalog.module("surveillance")) == 568345184 + 9763086);
  CHECK(catalog.module("surveillance")->dataOwners == std::vector<std::string>{"camera", "guard", "identity"});
  CHECK(catalog.module("surveillance")->gates.size() == 7);
}

TEST_CASE("the catalog refuses cycles, unknown references and unpinned downloads")
{
  CHECK(problemOf(catalogOf({module({.id = "core", .kind = "core", .required = {}}), module({.id = "a", .kind = "available", .required = {"b"}}),
                             module({.id = "b", .kind = "available", .required = {"a"}})}))
            .find("cycle") != std::string::npos);
  CHECK(problemOf(catalogOf({module({.id = "core", .kind = "core", .required = {}}), module({.id = "a", .kind = "available", .required = {"ghost"}})}))
            .find("unknown module ghost") != std::string::npos);
  CHECK(problemOf(catalogOf({module({.id = "core", .kind = "core", .required = {}}), module({.id = "core", .kind = "available", .required = {}})})).find("repeats") !=
        std::string::npos);
  CHECK(problemOf(catalogOf({module({.id = "a", .kind = "available", .required = {}})})).find("exactly one module is core") != std::string::npos);
  CHECK(problemOf(catalogOf({module({.id = "core", .kind = "core", .required = {}}), module({.id = "Bad Id", .kind = "available", .required = {}})})).find("lowercase") !=
        std::string::npos);

  auto withComponent = catalogOf({module({.id = "core", .kind = "core", .required = {}}), module({.id = "a", .kind = "available", .required = {"core"}})});
  withComponent["modules"][1]["components"].append("ghost");
  CHECK(problemOf(withComponent).find("unknown component ghost") != std::string::npos);

  auto root = shipped();
  root["components"][3]["files"][0]["url"] =
      "https://huggingface.co/LiquidAI/LFM2.5-VL-450M-GGUF/resolve/main/LFM2.5-VL-450M-Q8_0.gguf";
  CHECK(problemOf(root).find("pinned revision") != std::string::npos);

  root = shipped();
  root["components"][3]["files"][0].removeMember("sha256");
  CHECK(problemOf(root).find("SHA-256") != std::string::npos);

  root = shipped();
  root["components"][3]["files"][0]["path"] = "../escape.gguf";
  CHECK(problemOf(root).find("relative path") != std::string::npos);

  root = shipped();
  root["components"][4]["hostCommand"] = Json::Value(Json::nullValue);
  CHECK(problemOf(root).find("hostCommand") != std::string::npos);

  root = shipped();
  root["modules"][3]["components"].append("vision");
  CHECK(problemOf(root).find("coming_soon") != std::string::npos);

  root = shipped();
  root["modules"][1]["dataOwners"].append("billing");
  CHECK(problemOf(root).find("billing") != std::string::npos);

  root = shipped();
  root["modules"][2]["gates"].append("/camera");
  CHECK(problemOf(root).find("gated by two modules") != std::string::npos);
}

TEST_CASE("dependencies resolve deps-first and enabled dependants are found")
{
  const auto catalog = testCatalog();
  CHECK(module_resolver::installOrder(catalog, "b") == std::vector<std::string>{"core", "a", "b"});
  CHECK(module_resolver::installOrder(catalog, "core") == std::vector<std::string>{"core"});
  CHECK(module_resolver::enabledDependents(catalog, "a", {"core", "a", "b"}) == std::vector<std::string>{"b"});
  CHECK(module_resolver::enabledDependents(catalog, "a", {"core", "a"}).empty());
  CHECK(module_resolver::enabledDependents(catalog, "core", {"core", "a"}) == std::vector<std::string>{"a"});
  CHECK_FALSE(module_resolver::cycleThrough(catalog).has_value());
}

TEST_CASE("the hardware verdict counts the enabled modules and the remaining download")
{
  const auto catalog = testCatalog();
  const std::set<std::string> enabled{"core"};
  constexpr std::int64_t kMb = std::int64_t{1024} * 1024;
  const HostResources roomy{.ramTotalMb = 16000, .freeDiskBytes = 1000 * kMb, .cpuFeatures = {"avx2"}, .gpu = false};

  auto verdict = assessHardware(
      {.catalog = catalog, .module = *catalog.module("a"), .enabled = enabled, .remainingBytes = 100 * kMb, .host = roomy});
  CHECK(verdict.verdict == HardwareVerdict::Ok);
  CHECK(verdict.reasons.empty());
  CHECK(verdict.minRamMb == 1000);
  CHECK(verdict.freeDiskMb == 1000);

  const HostResources tight{.ramTotalMb = 9000, .freeDiskBytes = 1000 * kMb, .cpuFeatures = {}, .gpu = false};
  verdict = assessHardware(
      {.catalog = catalog, .module = *catalog.module("a"), .enabled = enabled, .remainingBytes = 0, .host = tight});
  CHECK(verdict.verdict == HardwareVerdict::Slow);
  CHECK(verdict.reasons == std::vector<std::string>{"ram_below_recommended", "cpu_feature_missing"});

  const HostResources small{.ramTotalMb = 4500, .freeDiskBytes = 1000 * kMb, .cpuFeatures = {"avx2"}, .gpu = false};
  verdict = assessHardware(
      {.catalog = catalog, .module = *catalog.module("a"), .enabled = enabled, .remainingBytes = 0, .host = small});
  CHECK(verdict.verdict == HardwareVerdict::Insufficient);
  CHECK(verdict.reasons == std::vector<std::string>{"ram_below_minimum"});

  verdict = assessHardware({.catalog = catalog,
                            .module = *catalog.module("a"),
                            .enabled = enabled,
                            .remainingBytes = 950 * kMb,
                            .host = roomy});
  CHECK(verdict.verdict == HardwareVerdict::Insufficient);
  CHECK(verdict.reasons == std::vector<std::string>{"disk_insufficient"});
  CHECK(diskNeededBytes(100) == 110);
  CHECK(diskNeededBytes(0) == 0);

  verdict = assessHardware(
      {.catalog = catalog, .module = *catalog.module("b"), .enabled = enabled, .remainingBytes = 0, .host = roomy});
  CHECK(verdict.verdict == HardwareVerdict::Insufficient);
  CHECK(verdict.reasons == std::vector<std::string>{"cpu_feature_missing", "gpu_missing"});

  const HostResources unknownDisk{.ramTotalMb = 16000, .freeDiskBytes = std::nullopt, .cpuFeatures = {"avx2"}, .gpu = false};
  verdict = assessHardware(
      {.catalog = catalog, .module = *catalog.module("a"), .enabled = enabled, .remainingBytes = 1, .host = unknownDisk});
  CHECK(verdict.verdict == HardwareVerdict::Ok);
  CHECK_FALSE(verdict.freeDiskMb.has_value());
}

TEST_CASE("progress frames are throttled to one a second and one percent, state changes always pass")
{
  ModuleEventThrottle throttle;
  CHECK(throttle.admit({.jobId = 1, .state = JobState::Queued, .progress = 0, .nowMs = 1000}));
  CHECK(throttle.admit({.jobId = 1, .state = JobState::Downloading, .progress = 0, .nowMs = 1001}));
  CHECK_FALSE(throttle.admit({.jobId = 1, .state = JobState::Downloading, .progress = 0.5, .nowMs = 1500}));
  CHECK_FALSE(throttle.admit({.jobId = 1, .state = JobState::Downloading, .progress = 0.005, .nowMs = 3000}));
  CHECK(throttle.admit({.jobId = 1, .state = JobState::Downloading, .progress = 0.5, .nowMs = 3000}));
  CHECK_FALSE(throttle.admit({.jobId = 1, .state = JobState::Downloading, .progress = 0.9, .nowMs = 3999}));
  CHECK(throttle.admit({.jobId = 2, .state = JobState::Downloading, .progress = 0.9, .nowMs = 3999}));
  CHECK(throttle.admit({.jobId = 1, .state = JobState::Verifying, .progress = 0.9, .nowMs = 4000}));
  CHECK(throttle.admit({.jobId = 1, .state = JobState::Done, .progress = 1, .nowMs = 4001}));
}

TEST_CASE("the uninstall body takes keepData and a numeric PIN and refuses anything else")
{
  const auto defaults = UninstallModuleDto::fromJson(parse("{}"));
  CHECK(defaults.keepData);
  CHECK_FALSE(defaults.pin.has_value());
  const auto purge = UninstallModuleDto::fromJson(parse(R"({"keepData": false, "pin": "2468"})"));
  CHECK_FALSE(purge.keepData);
  CHECK(purge.pin == "2468");
  CHECK_THROWS_AS(UninstallModuleDto::fromJson(parse(R"({"keepData": "no"})")), ValidationException);
  CHECK_THROWS_AS(UninstallModuleDto::fromJson(parse(R"({"keepData": false, "pin": "12a4"})")), ValidationException);
  CHECK_THROWS_AS(UninstallModuleDto::fromJson(parse(R"({"pin": "123456789012345678901234567890123"})")),
                  ValidationException);
}

TEST_CASE("the owner sees the whole module, every other role only id, name, enabled, lifecycle and purge stamp")
{
  const auto parsed = parseModuleCatalog(shipped());
  if (!parsed.catalog) {
    FAIL("the shipped catalog is invalid");
    return;
  }
  const auto& catalog = *parsed.catalog;
  const auto* surveillance = catalog.module("surveillance");
  ModuleView view{.module = surveillance,
                  .enabled = false,
                  .lifecycle = ModuleLifecycle::UninstalledDataKept,
                  .hasData = true,
                  .dataPurgedAt = 0,
                  .sizeBytes = catalog.sizeBytes(*surveillance),
                  .installedBytes = 10,
                  .hardware = {.verdict = HardwareVerdict::Slow,
                               .reasons = {"ram_below_recommended"},
                               .minRamMb = 1536,
                               .recommendedRamMb = 3072,
                               .freeDiskMb = 2048},
                  .job = JobView{.job = {.id = 9,
                                         .moduleId = "surveillance",
                                         .kind = JobKind::Purge,
                                         .owner = "guard",
                                         .state = JobState::Failed,
                                         .reason = "purge_unsupported",
                                         .bytesDone = 0,
                                         .bytesTotal = 0,
                                         .requestedBy = 1,
                                         .createdAt = 1,
                                         .updatedAt = 1,
                                         .stateSince = 1},
                                 .progress = 0,
                                 .bytesPerSecond = 0,
                                 .etaSeconds = std::nullopt},
                  .components = {}};

  const auto owner = ResponseListModulesDto{.modules = {view}, .owner = true, .lang = "en"}.toJson()["modules"][0];
  CHECK(owner["name"] == "Surveillance");
  CHECK(owner["lifecycle"] == "uninstalled_data_kept");
  CHECK(owner["hasData"] == true);
  CHECK(owner["dataPurgedAt"].isNull());
  CHECK(owner["requires"][0] == "core");
  CHECK(owner["hardware"]["verdict"] == "slow");
  CHECK(owner["hardware"]["freeDiskMb"] == 2048);
  CHECK(owner["job"]["kind"] == "purge");
  CHECK(owner["job"]["owner"] == "guard");
  CHECK(owner["job"]["reason"] == "purge_unsupported");
  CHECK(owner["job"]["etaSeconds"].isNull());
  CHECK(owner["gettingStarted"].size() == 3);
  CHECK(owner.isMember("components"));

  view.dataPurgedAt = 1234;
  const auto member = ResponseListModulesDto{.modules = {view}, .owner = false, .lang = "es"}.toJson()["modules"][0];
  CHECK(member["name"] == "Vigilancia");
  CHECK(member["enabled"] == false);
  CHECK(member["lifecycle"] == "uninstalled_data_kept");
  CHECK(member["dataPurgedAt"] == 1234);
  CHECK(member.getMemberNames().size() == 5);
}

TEST_CASE("the event payloads carry the kind, version, settled flag and either the module or the enabled set")
{
  const ModuleStatesReply set{.modules = {{.id = "core", .enabled = true, .lifecycle = "active", .dataPurgedAt = 0},
                                          {.id = "surveillance", .enabled = false, .lifecycle = "not_installed", .dataPurgedAt = 77}},
                              .version = 12,
                              .settled = true};
  Json::Value enabled = parse(module_event::enabledPayload(set, 99));
  CHECK(enabled["kind"] == "enabled");
  CHECK(enabled["version"] == 12);
  CHECK(enabled["settled"] == true);
  CHECK(enabled["at"] == 99);
  CHECK(enabled["modules"][1]["id"] == "surveillance");
  CHECK(enabled["modules"][1]["enabled"] == false);
  CHECK(enabled["modules"][1]["lifecycle"] == "not_installed");
  CHECK(enabled["modules"][1]["dataPurgedAt"] == 77);
  CHECK(enabled["modules"][0]["dataPurgedAt"].isNull());
  CHECK_FALSE(enabled.isMember("module"));

  const auto parsed = parseModuleCatalog(shipped());
  if (!parsed.catalog) {
    FAIL("the shipped catalog is invalid");
    return;
  }
  const ModuleView view{.module = parsed.catalog->module("productivity"),
                        .enabled = true,
                        .lifecycle = ModuleLifecycle::Active,
                        .hasData = false,
                        .dataPurgedAt = 0,
                        .sizeBytes = 0,
                        .installedBytes = 0,
                        .hardware = {},
                        .job = std::nullopt,
                        .components = {}};
  Json::Value moduleEvent = parse(module_event::modulePayload(view, set, 5));
  CHECK(moduleEvent["kind"] == "module");
  CHECK(moduleEvent["module"]["id"] == "productivity");
  CHECK(moduleEvent["module"]["name"] == "Productividad");
  CHECK(moduleEvent["module"]["job"].isNull());
  CHECK_FALSE(moduleEvent.isMember("modules"));
}
