#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <drogon/orm/DbClient.h>
#include <errors/response-exception.hxx>
#include <errors/validation-exception.hxx>
#include <feature/modules/infra/module-catalog-file.hxx>
#include <feature/modules/services/module-engine.hxx>
#include <feature/modules/services/module-journal.hxx>
#include <json/reader.h>
#include <sqlite/db-service.hxx>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

namespace
{
namespace fs = std::filesystem;

constexpr const char* kSha = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
constexpr const char* kRevision = "1abed04b6fe71314d8c446a1371c03d7c332266d";

struct ImpactState
{
  OwnerReach reach{OwnerReach::Answered};
  ModuleImpactReport report;
};

struct OwnerState
{
  OwnerReach reach{OwnerReach::Answered};
  std::map<std::string, ComponentStatus> components;
  std::map<std::string, ModuleDataSummary> data;
  OwnerReach purgeReach{OwnerReach::Answered};
  bool purgeWorks{true};
};

class FakeOwners final : public ComponentOwners
{
public:
  mutable std::mutex mutex;
  std::map<std::string, OwnerState> owners;
  mutable std::vector<std::string> calls;
  OwnerReply<PinVerdict> pin{.reach = OwnerReach::Unsupported, .value = std::nullopt};
  std::map<std::string, ImpactState> impacts;
  OwnerReply<RoleReassignmentOutcome> reassignReply{.reach = OwnerReach::Unsupported, .value = std::nullopt};
  OwnerReply<ModuleRequestOutcome> requestReply{.reach = OwnerReach::Unsupported, .value = std::nullopt};
  mutable RoleReassignmentBatch lastBatch;
  mutable ModuleRequestInput lastRequest;

  void set(const std::string& owner, const ComponentStatus& status)
  {
    const std::scoped_lock lock(mutex);
    owners[owner].components[status.id] = status;
  }

  [[nodiscard]] std::vector<std::string> callsOf(const std::string& prefix) const
  {
    const std::scoped_lock lock(mutex);
    std::vector<std::string> matching;
    for (const auto& call : calls)
      if (call.starts_with(prefix))
        matching.push_back(call);
    return matching;
  }

  [[nodiscard]] OwnerStates states(const std::string& owner, const std::vector<ComponentSpec>& specs) const override
  {
    const std::scoped_lock lock(mutex);
    const auto found = owners.find(owner);
    if (found == owners.end() || found->second.reach != OwnerReach::Answered)
      return {.reach = found == owners.end() ? OwnerReach::Unsupported : found->second.reach, .states = {}};
    OwnerStates states{.reach = OwnerReach::Answered, .states = {}};
    for (const auto& spec : specs) {
      const auto status = found->second.components.find(spec.id);
      states.states.push_back(status != found->second.components.end()
                                  ? status->second
                                  : ComponentStatus{.id = spec.id,
                                                    .state = ComponentState::Missing,
                                                    .bytesPresent = 0,
                                                    .bytesTotal = spec.totalBytes(),
                                                    .ready = false,
                                                    .hostCommand = {},
                                                    .reason = {}});
    }
    return states;
  }

  OwnerReply<ComponentStatus> record(const std::string& verb, const std::string& owner, const ComponentSpec& spec,
                                     ComponentState next) const
  {
    const std::scoped_lock lock(mutex);
    calls.push_back(verb + ":" + owner + ":" + spec.id);
    auto found = owners.find(owner);
    if (found == owners.end())
      return {.reach = OwnerReach::Unsupported, .value = std::nullopt};
    if (found->second.reach != OwnerReach::Answered)
      return {.reach = found->second.reach, .value = std::nullopt};
    auto& status = const_cast<OwnerState&>(found->second).components[spec.id];
    status.id = spec.id;
    status.bytesTotal = spec.totalBytes();
    if (next == ComponentState::Missing)
      status.bytesPresent = 0;
    if (!(next == ComponentState::Installing && status.state == ComponentState::Installed))
      status.state = next;
    return {.reach = OwnerReach::Answered, .value = status};
  }

  [[nodiscard]] OwnerReply<ComponentStatus> install(const std::string& owner, const ComponentSpec& spec) const override
  {
    return record("install", owner, spec, ComponentState::Installing);
  }

  [[nodiscard]] OwnerReply<ComponentStatus> cancel(const std::string& owner, const ComponentSpec& spec) const override
  {
    const std::scoped_lock lock(mutex);
    calls.push_back("cancel:" + owner + ":" + spec.id);
    return {.reach = OwnerReach::Answered, .value = std::nullopt};
  }

  [[nodiscard]] OwnerReply<ComponentStatus> remove(const std::string& owner, const ComponentSpec& spec) const override
  {
    return record("remove", owner, spec, ComponentState::Missing);
  }

  [[nodiscard]] OwnerReply<ModuleDataSummary> dataSummary(const std::string& owner,
                                                          const std::string& moduleId) const override
  {
    const std::scoped_lock lock(mutex);
    calls.push_back("data:" + owner + ":" + moduleId);
    const auto found = owners.find(owner);
    if (found == owners.end())
      return {.reach = OwnerReach::Unsupported, .value = std::nullopt};
    const auto data = found->second.data.find(moduleId);
    return {.reach = OwnerReach::Answered,
            .value = data == found->second.data.end() ? ModuleDataSummary{} : data->second};
  }

  [[nodiscard]] OwnerReply<ModuleDataPurge> purgeData(const std::string& owner, const std::string& moduleId) const override
  {
    const std::scoped_lock lock(mutex);
    calls.push_back("purge:" + owner + ":" + moduleId);
    const auto found = owners.find(owner);
    if (found == owners.end())
      return {.reach = OwnerReach::Unsupported, .value = std::nullopt};
    if (found->second.purgeReach != OwnerReach::Answered)
      return {.reach = found->second.purgeReach, .value = std::nullopt};
    if (found->second.purgeWorks)
      const_cast<OwnerState&>(found->second).data.erase(moduleId);
    return {.reach = OwnerReach::Answered, .value = ModuleDataPurge{.purged = found->second.purgeWorks, .reason = {}}};
  }

  [[nodiscard]] OwnerReply<PinVerdict> verifyPin(const std::string& owner, const OwnerPinCheck& check) const override
  {
    const std::scoped_lock lock(mutex);
    calls.push_back("pin:" + owner + ":" + check.pin);
    return pin;
  }

  [[nodiscard]] OwnerReply<ModuleImpactReport> impact(const std::string& owner, const std::string& moduleId) const override
  {
    const std::scoped_lock lock(mutex);
    calls.push_back("impact:" + owner + ":" + moduleId);
    const auto found = impacts.find(owner);
    if (found == impacts.end())
      return {.reach = OwnerReach::Unsupported, .value = std::nullopt};
    if (found->second.reach != OwnerReach::Answered)
      return {.reach = found->second.reach, .value = std::nullopt};
    return {.reach = OwnerReach::Answered, .value = found->second.report};
  }

  [[nodiscard]] OwnerReply<RoleReassignmentOutcome> reassignRoles(const std::string& owner,
                                                                  const RoleReassignmentBatch& batch) const override
  {
    const std::scoped_lock lock(mutex);
    std::string text = "reassign:" + owner + ":" + std::to_string(batch.actorUserId);
    for (const auto& entry : batch.reassignments)
      text += ":" + std::to_string(entry.userId) + "=" + entry.role;
    calls.push_back(text);
    lastBatch = batch;
    return reassignReply;
  }

  [[nodiscard]] OwnerReply<ModuleRequestOutcome> requestModule(const std::string& owner,
                                                               const ModuleRequestInput& input) const override
  {
    const std::scoped_lock lock(mutex);
    calls.push_back("request:" + owner + ":" + input.moduleId + ":" + std::to_string(input.userId));
    lastRequest = input;
    return requestReply;
  }
};

class ActionSink final : public ModuleActionSink
{
public:
  std::vector<ModuleActionRecord> records;

  bool publish(const ModuleActionRecord& record) override
  {
    records.push_back(record);
    return true;
  }
};

class FakeSink final : public ModuleEventSink
{
public:
  std::mutex mutex;
  std::vector<std::string> frames;
  std::vector<ModuleStatesReply> sets;

  bool moduleChanged(const ModuleView& view, const ModuleStatesReply&) override
  {
    const std::scoped_lock lock(mutex);
    frames.push_back(view.module->id + ":" + (view.job ? std::string(jobStateToString(view.job->job.state)) : "-"));
    return true;
  }

  bool enabledChanged(const ModuleStatesReply& set) override
  {
    const std::scoped_lock lock(mutex);
    sets.push_back(set);
    return true;
  }
};

struct ComponentInput
{
  std::string id;
  std::string owner;
  std::string source;
};

Json::Value component(const ComponentInput& input)
{
  const auto& [id, owner, source] = input;
  Json::Value json(Json::objectValue);
  json["id"] = id;
  json["owner"] = owner;
  json["source"] = source;
  json["ramMb"] = 100;
  Json::Value file(Json::objectValue);
  file["path"] = id + "/model.bin";
  file["sizeBytes"] = 1000;
  if (source == "download") {
    file["url"] = std::string("https://example.test/") + kRevision + "/model.bin";
    file["sha256"] = kSha;
  }
  else {
    json["hostCommand"] = "services/" + owner + "/scripts/provision.sh";
  }
  json["files"].append(file);
  return json;
}

struct ModuleInput
{
  std::string id;
  std::string kind;
  std::vector<std::string> required;
  std::vector<std::string> components;
};

Json::Value module(const ModuleInput& input)
{
  const auto& [id, kind, required, components] = input;
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
  for (const auto& entry : components)
    json["components"].append(entry);
  json["gates"] = Json::Value(Json::arrayValue);
  json["dataOwners"] = Json::Value(Json::arrayValue);
  json["hardware"]["minRamMb"] = 1000;
  json["hardware"]["recommendedRamMb"] = 2000;
  json["gettingStarted"] = Json::Value(Json::arrayValue);
  return json;
}

ModuleCatalog testCatalog()
{
  Json::Value root(Json::objectValue);
  root["components"].append(component({.id = "vision", .owner = "vlm", .source = "download"}));
  root["components"].append(component({.id = "detector", .owner = "camera", .source = "provisioned"}));
  root["components"].append(component({.id = "voice", .owner = "tts", .source = "provisioned"}));
  root["components"].append(component({.id = "insight", .owner = "llm", .source = "download"}));
  root["modules"].append(module({.id = "core", .kind = "core", .required = {}, .components = {"voice"}}));
  auto surveillance = module({.id = "surveillance", .kind = "available", .required = {"core"}, .components = {"detector", "vision"}});
  surveillance["dataOwners"].append("camera");
  surveillance["dataOwners"].append("guard");
  surveillance["roles"].append("guard");
  surveillance["settingsOwners"].append("camera");
  surveillance["settingsOwners"].append("guard");
  for (const auto* effect : {"live_views", "camera_talk", "guard_duty", "pending_alerts"})
    surveillance["effects"].append(effect);
  Json::Value safety(Json::objectValue);
  safety["id"] = "safety_alerts";
  safety["text"]["es"] = "Las alertas de pánico seguirán.";
  safety["text"]["en"] = "Panic alerts keep going.";
  surveillance["keepsRunning"].append(safety);
  root["modules"].append(surveillance);
  root["modules"].append(module({.id = "productivity", .kind = "available", .required = {"core"}, .components = {}}));
  root["modules"].append(module({.id = "insights", .kind = "available", .required = {"surveillance"}, .components = {"insight"}}));
  root["modules"].append(module({.id = "agronomy", .kind = "coming_soon", .required = {"core"}, .components = {}}));
  auto parsed = parseModuleCatalog(root);
  INFO(parsed.problem);
  CHECK(parsed.catalog.has_value());
  return std::move(parsed.catalog).value_or(ModuleCatalog{});
}

ComponentStatus installed(const std::string& id, bool ready = true)
{
  return {.id = id,
          .state = ComponentState::Installed,
          .bytesPresent = 1000,
          .bytesTotal = 1000,
          .ready = ready,
          .hostCommand = {},
          .reason = {}};
}

ComponentStatus partial(const std::string& id, std::int64_t bytes)
{
  return {.id = id,
          .state = ComponentState::Installing,
          .bytesPresent = bytes,
          .bytesTotal = 1000,
          .ready = false,
          .hostCommand = {},
          .reason = {}};
}

struct Harness
{
  fs::path path;
  drogon::orm::DbClientPtr db;
  FakeOwners owners;
  FakeSink sink;
  std::atomic<std::int64_t> now{1000000};
  HostResources host{.ramTotalMb = 16000, .freeDiskBytes = 1099511627776, .cpuFeatures = {"avx2"}, .gpu = false};
  std::unique_ptr<ModuleEngine> engine;

  Harness()
      : path(fs::temp_directory_path() /
             ("argus-module-engine-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
              ".db"))
  {
    db = drogon::orm::DbClient::newSqlite3Client("filename=" + path.string(), 1);
    REQUIRE(DbService::runScriptFile(ARGUS_SETTINGS_SCHEMA_FILE, db));
    owners.owners["tts"].components["voice"] = installed("voice");
    owners.owners["camera"].components["detector"] = installed("detector");
    owners.owners["vlm"];
    owners.owners["guard"];
    owners.owners["llm"];
    boot();
  }

  ~Harness()
  {
    engine.reset();
    db.reset();
    std::error_code error;
    fs::remove(path, error);
    fs::remove(path.string() + "-wal", error);
    fs::remove(path.string() + "-shm", error);
  }

  Harness(const Harness&) = delete;
  Harness& operator=(const Harness&) = delete;

  void boot()
  {
    engine.reset();
    engine = std::make_unique<ModuleEngine>(ModuleEngineInput{
        .catalog = testCatalog(),
        .db = db,
        .owners = owners,
        .events = &sink,
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
  }

  void step(int times = 1)
  {
    for (int index = 0; index < times; ++index) {
      now += 1000;
      engine->tick();
    }
  }

  [[nodiscard]] ModuleView view(const std::string& id) const { return engine->view(id); }

  [[nodiscard]] JobView job(const std::string& id) const
  {
    const auto current = engine->view(id);
    CHECK(current.job.has_value());
    return current.job.value_or(JobView{});
  }

  [[nodiscard]] JobState state(const std::string& id) const { return job(id).job.state; }

  [[nodiscard]] bool enabled(const std::string& id) const
  {
    for (const auto& module : engine->enabledSet().modules)
      if (module.id == id)
        return module.enabled;
    return false;
  }
};

int statusOf(const std::function<void()>& call)
{
  try {
    call();
  }
  catch (const ResponseException& error) {
    return error.statusCode();
  }
  return 0;
}

std::string codeOf(const std::function<void()>& call)
{
  try {
    call();
  }
  catch (const ResponseException& error) {
    return error.errorCode();
  }
  return {};
}
}

TEST_CASE("the first boot adopts what is installed and publishes the settled set")
{
  Harness harness;
  CHECK_FALSE(harness.engine->enabledSet().settled);
  CHECK(statusOf([&] { static_cast<void>(harness.engine->install({.moduleId = "productivity", .userId = 1})); }) == 503);

  harness.step();
  const auto set = harness.engine->enabledSet();
  CHECK(set.settled);
  CHECK(set.version > 0);
  CHECK(harness.enabled("core"));
  CHECK(harness.enabled("productivity"));
  CHECK_FALSE(harness.enabled("surveillance"));
  CHECK_FALSE(harness.enabled("agronomy"));
  CHECK(harness.view("surveillance").lifecycle == ModuleLifecycle::NotInstalled);
  CHECK(harness.view("productivity").lifecycle == ModuleLifecycle::Active);
  REQUIRE_FALSE(harness.sink.sets.empty());
  CHECK(harness.sink.sets.back().version == set.version);

  harness.boot();
  CHECK(harness.engine->enabledSet().settled);
  CHECK(harness.engine->enabledSet().version == set.version);
}

TEST_CASE("every boot of the engine mints its own epoch, and every event and reply of that boot carries it")
{
  Harness harness;
  harness.step();
  const auto first = harness.engine->enabledSet();
  REQUIRE_FALSE(first.epoch.empty());
  CHECK(first.epoch.starts_with(std::to_string(harness.now.load() - 1000) + "-"));
  REQUIRE_FALSE(harness.sink.sets.empty());
  for (const auto& set : harness.sink.sets)
    CHECK(set.epoch == first.epoch);

  harness.boot();
  const auto second = harness.engine->enabledSet();
  CHECK(second.version == first.version);
  REQUIRE_FALSE(second.epoch.empty());
  CHECK(second.epoch != first.epoch);
  CHECK(harness.engine->enabledSet().epoch == second.epoch);
}

TEST_CASE("an owner that cannot be reached holds the seed back until the wait ends, then counts as installed")
{
  Harness harness;
  harness.owners.owners["vlm"].reach = OwnerReach::Unreachable;
  harness.step();
  CHECK_FALSE(harness.engine->enabledSet().settled);
  harness.now += 300000;
  harness.engine->tick();
  CHECK(harness.engine->enabledSet().settled);
  CHECK(harness.enabled("surveillance"));
}

TEST_CASE("an install downloads, verifies, activates and confirms the model loaded, with progress that never decreases")
{
  Harness harness;
  harness.step();
  const auto job = harness.engine->install({.moduleId = "surveillance", .userId = 7});
  CHECK(job.job.state == JobState::Queued);
  CHECK(job.job.kind == JobKind::Install);
  CHECK(job.job.bytesTotal == 2000);

  harness.step();
  CHECK(harness.state("surveillance") == JobState::Downloading);
  CHECK(harness.owners.callsOf("install:vlm:vision").size() == 1);

  harness.owners.set("vlm", partial("vision", 400));
  harness.step();
  CHECK(harness.job("surveillance").job.bytesDone == 1400);
  harness.owners.set("vlm", partial("vision", 100));
  harness.step();
  CHECK(harness.job("surveillance").job.bytesDone == 1400);
  harness.owners.set("vlm", partial("vision", 800));
  harness.step();
  const auto moving = harness.job("surveillance");
  CHECK(moving.job.bytesDone == 1800);
  CHECK(moving.progress == doctest::Approx(0.9));
  CHECK(moving.bytesPerSecond > 0);
  CHECK(moving.etaSeconds.has_value());

  harness.owners.set("vlm", installed("vision", false));
  harness.step();
  CHECK(harness.state("surveillance") == JobState::HealthCheck);
  CHECK(harness.enabled("surveillance"));
  CHECK(harness.view("surveillance").lifecycle == ModuleLifecycle::Active);

  harness.owners.set("vlm", installed("vision", true));
  harness.step();
  CHECK_FALSE(harness.view("surveillance").job.has_value());
  CHECK(harness.view("surveillance").installedBytes == 2000);
  CHECK(harness.engine->enabledSet().version > 0);
  CHECK(statusOf([&] { static_cast<void>(harness.engine->install({.moduleId = "surveillance", .userId = 7})); }) ==
        409);
}

TEST_CASE("an unfinished job resumes at boot from where it was and finishes")
{
  Harness harness;
  harness.step();
  static_cast<void>(harness.engine->install({.moduleId = "surveillance", .userId = 7}));
  harness.step();
  harness.owners.set("vlm", partial("vision", 500));
  harness.step();
  CHECK(harness.job("surveillance").job.bytesDone == 1500);

  harness.boot();
  const auto resumed = harness.job("surveillance");
  CHECK(resumed.job.state == JobState::Queued);
  CHECK(resumed.job.bytesDone == 1500);

  harness.owners.set("vlm", installed("vision"));
  harness.step(2);
  CHECK(harness.enabled("surveillance"));
  CHECK_FALSE(harness.view("surveillance").job.has_value());
}

TEST_CASE("a model that never loads rolls the module back and the job fails with health_check_failed")
{
  Harness harness;
  harness.owners.set("vlm", installed("vision", false));
  harness.owners.owners["camera"].components.erase("detector");
  harness.step();
  CHECK_FALSE(harness.enabled("surveillance"));
  harness.owners.set("camera", installed("detector", false));
  static_cast<void>(harness.engine->install({.moduleId = "surveillance", .userId = 7}));
  harness.step();
  CHECK(harness.state("surveillance") == JobState::HealthCheck);
  CHECK(harness.enabled("surveillance"));
  harness.now += 61000;
  harness.engine->tick();
  CHECK(harness.state("surveillance") == JobState::Failed);
  CHECK(harness.job("surveillance").job.reason == "health_check_failed");
  CHECK_FALSE(harness.enabled("surveillance"));
  CHECK(harness.view("surveillance").lifecycle == ModuleLifecycle::Disabled);
}

TEST_CASE("a provisioned component the host has not produced fails the job with host_only")
{
  Harness harness;
  harness.owners.owners["camera"].components["detector"] = {.id = "detector",
                                                            .state = ComponentState::HostOnly,
                                                            .bytesPresent = 0,
                                                            .bytesTotal = 1000,
                                                            .ready = false,
                                                            .hostCommand = "services/camera/scripts/provision.sh",
                                                            .reason = {}};
  harness.step();
  static_cast<void>(harness.engine->install({.moduleId = "surveillance", .userId = 7}));
  harness.step();
  CHECK(harness.state("surveillance") == JobState::Failed);
  CHECK(harness.job("surveillance").job.reason == "host_only");
  CHECK(harness.view("surveillance").components[0].status.state == ComponentState::HostOnly);
}

TEST_CASE("a component that fails passes its owner's reason to the job")
{
  Harness harness;
  harness.step();
  static_cast<void>(harness.engine->install({.moduleId = "surveillance", .userId = 7}));
  harness.step();
  harness.owners.set("vlm", {.id = "vision",
                             .state = ComponentState::Failed,
                             .bytesPresent = 10,
                             .bytesTotal = 1000,
                             .ready = false,
                             .hostCommand = {},
                             .reason = "checksum_mismatch"});
  harness.step();
  CHECK(harness.state("surveillance") == JobState::Failed);
  CHECK(harness.job("surveillance").job.reason == "checksum_mismatch");
  CHECK_FALSE(harness.owners.callsOf("cancel:vlm:vision").empty());
}

TEST_CASE("the install refusals: unknown, coming soon, already enabled, running job and insufficient hardware")
{
  Harness harness;
  harness.step();
  CHECK(statusOf([&] { static_cast<void>(harness.engine->install({.moduleId = "ghost", .userId = 1})); }) == 404);
  CHECK(codeOf([&] { static_cast<void>(harness.engine->install({.moduleId = "agronomy", .userId = 1})); }) ==
        "MODULE_COMING_SOON");
  CHECK(statusOf([&] { static_cast<void>(harness.engine->install({.moduleId = "core", .userId = 1})); }) == 409);
  static_cast<void>(harness.engine->install({.moduleId = "surveillance", .userId = 1}));
  CHECK(codeOf([&] { static_cast<void>(harness.engine->install({.moduleId = "surveillance", .userId = 1})); }) ==
        "MODULE_JOB_RUNNING");
  static_cast<void>(harness.engine->cancel({.moduleId = "surveillance", .userId = 1}));

  harness.host.ramTotalMb = 1500;
  CHECK(codeOf([&] { static_cast<void>(harness.engine->install({.moduleId = "surveillance", .userId = 1})); }) ==
        "MODULE_HARDWARE_INSUFFICIENT");
  CHECK(harness.view("surveillance").hardware.verdict == HardwareVerdict::Insufficient);
}

TEST_CASE("installing a module queues what it requires first and runs one job at a time")
{
  Harness harness;
  harness.step();
  const auto job = harness.engine->install({.moduleId = "insights", .userId = 3});
  CHECK(harness.state("surveillance") == JobState::Queued);
  CHECK(job.job.state == JobState::Queued);
  harness.step();
  CHECK(harness.state("surveillance") == JobState::Downloading);
  CHECK(harness.state("insights") == JobState::Queued);
  harness.owners.set("vlm", installed("vision"));
  harness.step();
  CHECK(harness.enabled("surveillance"));
  CHECK(harness.state("insights") == JobState::Downloading);
  harness.owners.set("llm", installed("insight"));
  harness.step();
  CHECK(harness.enabled("insights"));

  CHECK(codeOf([&] { static_cast<void>(harness.engine->disable({.moduleId = "surveillance", .userId = 3})); }) ==
        "MODULE_REQUIRED_BY");
  CHECK(codeOf([&] { static_cast<void>(harness.engine->disable({.moduleId = "core", .userId = 3})); }) ==
        "MODULE_CORE");
  const auto disabled = harness.engine->disable({.moduleId = "insights", .userId = 3});
  CHECK_FALSE(disabled.enabled);
  CHECK(disabled.lifecycle == ModuleLifecycle::Disabled);
  const auto again = harness.engine->install({.moduleId = "insights", .userId = 3});
  CHECK(again.job.state == JobState::Queued);
  harness.step();
  CHECK(harness.enabled("insights"));
}

TEST_CASE("a dependant fails when what it requires failed")
{
  Harness harness;
  harness.owners.owners["camera"].components["detector"].state = ComponentState::HostOnly;
  harness.owners.owners["camera"].components["detector"].bytesPresent = 0;
  harness.step();
  static_cast<void>(harness.engine->install({.moduleId = "insights", .userId = 3}));
  harness.step(2);
  CHECK(harness.state("surveillance") == JobState::Failed);
  CHECK(harness.state("insights") == JobState::Failed);
  CHECK(harness.job("insights").job.reason == "dependency_failed");
}

TEST_CASE("pause stops the owner's fetch, resume queues again, cancel ends the job")
{
  Harness harness;
  harness.step();
  static_cast<void>(harness.engine->install({.moduleId = "surveillance", .userId = 1}));
  harness.step();
  CHECK(harness.state("surveillance") == JobState::Downloading);
  const auto paused = harness.engine->pause({.moduleId = "surveillance", .userId = 1});
  CHECK(paused.job.state == JobState::Paused);
  harness.step();
  CHECK(harness.owners.callsOf("cancel:vlm:vision").size() == 1);
  CHECK(harness.state("surveillance") == JobState::Paused);

  CHECK(harness.engine->resume({.moduleId = "surveillance", .userId = 1}).job.state == JobState::Queued);
  harness.step();
  CHECK(harness.state("surveillance") == JobState::Downloading);
  CHECK(harness.engine->cancel({.moduleId = "surveillance", .userId = 1}).job.state == JobState::Cancelled);
  CHECK(statusOf([&] { static_cast<void>(harness.engine->pause({.moduleId = "surveillance", .userId = 1})); }) ==
        404);
  CHECK(statusOf([&] { static_cast<void>(harness.engine->cancel({.moduleId = "productivity", .userId = 1})); }) ==
        404);
}

TEST_CASE("a step that cannot change waits: an owner in health check refuses pause and cancel")
{
  Harness harness;
  harness.owners.set("vlm", installed("vision", false));
  harness.owners.owners["camera"].components.erase("detector");
  harness.step();
  harness.owners.set("camera", installed("detector"));
  static_cast<void>(harness.engine->install({.moduleId = "surveillance", .userId = 1}));
  harness.step();
  CHECK(harness.state("surveillance") == JobState::HealthCheck);
  CHECK(statusOf([&] { static_cast<void>(harness.engine->pause({.moduleId = "surveillance", .userId = 1})); }) ==
        409);
  CHECK(statusOf([&] { static_cast<void>(harness.engine->cancel({.moduleId = "surveillance", .userId = 1})); }) ==
        409);
}

TEST_CASE("an owner that predates the component calls is trusted as provisioned")
{
  Harness harness;
  harness.owners.owners.erase("vlm");
  harness.owners.owners["camera"].components.erase("detector");
  harness.owners.owners.erase("camera");
  harness.step();
  CHECK(harness.enabled("surveillance"));
  static_cast<void>(harness.engine->disable({.moduleId = "surveillance", .userId = 1}));
  static_cast<void>(harness.engine->install({.moduleId = "surveillance", .userId = 1}));
  harness.step();
  CHECK(harness.enabled("surveillance"));
  CHECK(harness.view("surveillance").installedBytes == 2000);
}

TEST_CASE("uninstall keeping data removes the files and leaves the data, reinstall brings the module back")
{
  Harness harness;
  harness.owners.set("vlm", installed("vision"));
  harness.owners.owners["camera"].data["surveillance"] = {.items = {{.kind = "camera", .count = 2}}, .bytes = 0};
  harness.step();
  CHECK(harness.enabled("surveillance"));
  CHECK(harness.view("surveillance").hasData);

  CHECK(codeOf([&] { static_cast<void>(harness.engine->uninstall({.moduleId = "core", .userId = 1, .keepData = true, .pin = {}})); }) ==
        "MODULE_CORE");
  const auto job = harness.engine->uninstall({.moduleId = "surveillance", .userId = 1, .keepData = true, .pin = {}});
  CHECK(job.job.kind == JobKind::Uninstall);
  CHECK_FALSE(harness.enabled("surveillance"));
  CHECK(codeOf([&] { static_cast<void>(harness.engine->uninstall({.moduleId = "surveillance", .userId = 1, .keepData = true, .pin = {}})); }) ==
        "MODULE_JOB_RUNNING");
  harness.step(2);
  CHECK(harness.owners.callsOf("remove:vlm:vision").size() == 1);
  CHECK(harness.owners.callsOf("remove:camera").empty());
  const auto view = harness.view("surveillance");
  CHECK(view.lifecycle == ModuleLifecycle::UninstalledDataKept);
  CHECK(view.hasData);
  CHECK(harness.owners.callsOf("purge:").empty());

  static_cast<void>(harness.engine->install({.moduleId = "surveillance", .userId = 1}));
  harness.step();
  CHECK(harness.state("surveillance") == JobState::Downloading);
  harness.owners.set("vlm", installed("vision"));
  harness.step();
  CHECK(harness.view("surveillance").lifecycle == ModuleLifecycle::Active);
}

TEST_CASE("uninstall is refused while an enabled module requires the target")
{
  Harness harness;
  harness.owners.set("vlm", installed("vision"));
  harness.step();
  harness.owners.set("llm", installed("insight"));
  static_cast<void>(harness.engine->install({.moduleId = "insights", .userId = 1}));
  harness.step();
  CHECK(harness.enabled("insights"));
  const auto status = statusOf([&] {
    static_cast<void>(harness.engine->uninstall({.moduleId = "surveillance", .userId = 1, .keepData = true, .pin = {}}));
  });
  CHECK(status == 409);
  CHECK(codeOf([&] {
          static_cast<void>(
              harness.engine->uninstall({.moduleId = "surveillance", .userId = 1, .keepData = true, .pin = {}}));
        }) == "MODULE_REQUIRED_BY");
}

TEST_CASE("a purge asks the guard for the owner's PIN and maps its verdict")
{
  Harness harness;
  harness.owners.set("vlm", installed("vision"));
  harness.step();
  const UninstallCommand purge{.moduleId = "surveillance", .userId = 1, .keepData = false, .pin = std::nullopt};

  harness.owners.pin = {.reach = OwnerReach::Answered, .value = PinVerdict::Required};
  CHECK(codeOf([&] { static_cast<void>(harness.engine->uninstall(purge)); }) == "PIN_REQUIRED");
  CHECK(statusOf([&] { static_cast<void>(harness.engine->uninstall(purge)); }) == 403);
  harness.owners.pin = {.reach = OwnerReach::Answered, .value = PinVerdict::Invalid};
  CHECK(codeOf([&] { static_cast<void>(harness.engine->uninstall(purge)); }) == "PIN_INVALID");
  harness.owners.pin = {.reach = OwnerReach::Answered, .value = PinVerdict::Locked};
  CHECK(statusOf([&] { static_cast<void>(harness.engine->uninstall(purge)); }) == 429);
  harness.owners.pin = {.reach = OwnerReach::Unreachable, .value = std::nullopt};
  CHECK(statusOf([&] { static_cast<void>(harness.engine->uninstall(purge)); }) == 503);
  CHECK(harness.enabled("surveillance"));
  CHECK(harness.owners.callsOf("pin:guard").size() == 5);

  harness.owners.pin = {.reach = OwnerReach::Answered, .value = PinVerdict::Accepted};
  const auto job = harness.engine->uninstall(
      {.moduleId = "surveillance", .userId = 1, .keepData = false, .pin = std::string("2468")});
  CHECK(job.job.kind == JobKind::Purge);
  CHECK(harness.owners.callsOf("pin:guard:2468").size() == 1);
}

TEST_CASE("a purge that fails on one owner names it and a retry purges only the owners left")
{
  Harness harness;
  harness.owners.set("vlm", installed("vision"));
  harness.owners.owners["camera"].data["surveillance"] = {.items = {{.kind = "event", .count = 5}}, .bytes = 10};
  harness.owners.owners["guard"].purgeReach = OwnerReach::Unsupported;
  harness.step();
  static_cast<void>(harness.engine->uninstall({.moduleId = "surveillance", .userId = 1, .keepData = false, .pin = {}}));
  harness.step(3);
  const auto failed = harness.view("surveillance");
  const auto failedJob = harness.job("surveillance");
  CHECK(failedJob.job.state == JobState::Failed);
  CHECK(failedJob.job.reason == "purge_unsupported");
  CHECK(failedJob.job.owner == "guard");
  CHECK(failed.lifecycle == ModuleLifecycle::Disabled);
  CHECK(failed.dataPurgedAt == 0);
  CHECK(harness.owners.callsOf("purge:camera:surveillance").size() == 1);

  harness.owners.owners["guard"].purgeReach = OwnerReach::Answered;
  static_cast<void>(harness.engine->uninstall({.moduleId = "surveillance", .userId = 1, .keepData = false, .pin = {}}));
  harness.step(3);
  const auto purged = harness.view("surveillance");
  CHECK_FALSE(purged.job.has_value());
  CHECK(purged.lifecycle == ModuleLifecycle::NotInstalled);
  CHECK(purged.dataPurgedAt > 0);
  CHECK_FALSE(purged.hasData);
  CHECK(harness.owners.callsOf("purge:camera:surveillance").size() == 1);
  CHECK(harness.owners.callsOf("purge:guard:surveillance").size() == 2);
  bool stamped = false;
  for (const auto& module : harness.engine->enabledSet().modules)
    if (module.id == "surveillance")
      stamped = module.dataPurgedAt == purged.dataPurgedAt && module.lifecycle == "not_installed";
  CHECK(stamped);

  harness.boot();
  CHECK(harness.view("surveillance").dataPurgedAt == purged.dataPurgedAt);
}

TEST_CASE("the data summary lists every data owner with what it reported")
{
  Harness harness;
  harness.owners.owners["camera"].data["surveillance"] = {.items = {{.kind = "camera", .count = 3}}, .bytes = 2048};
  harness.step();
  const auto data = harness.engine->moduleData("surveillance");
  REQUIRE(data.size() == 2);
  CHECK(data[0].owner == "camera");
  CHECK(data[0].reach == OwnerReach::Answered);
  CHECK(data[0].summary.items[0].count == 3);
  CHECK(data[1].owner == "guard");
  CHECK(data[1].summary.empty());
  CHECK(harness.engine->moduleData("productivity").empty());
  CHECK(statusOf([&] { static_cast<void>(harness.engine->moduleData("ghost")); }) == 404);
}

TEST_CASE("progress frames reach the sink throttled while state changes always do")
{
  Harness harness;
  harness.step();
  static_cast<void>(harness.engine->install({.moduleId = "surveillance", .userId = 1}));
  harness.step();
  harness.owners.set("vlm", partial("vision", 1));
  harness.step();
  const auto before = harness.sink.frames.size();
  for (int bytes = 2; bytes <= 9; ++bytes) {
    harness.owners.set("vlm", partial("vision", bytes));
    harness.step();
  }
  CHECK(harness.sink.frames.size() == before);
  harness.owners.set("vlm", partial("vision", 500));
  harness.step();
  CHECK(harness.sink.frames.size() == before + 1);
  harness.owners.set("vlm", installed("vision"));
  harness.step();
  CHECK(harness.sink.frames.size() > before + 1);
  CHECK(harness.sink.frames.back() == "surveillance:done");
}

TEST_CASE("the enabled set answers enabled only for an active module and carries every lifecycle")
{
  Harness harness;
  harness.owners.set("vlm", installed("vision"));
  harness.step();
  const auto lifecycleOf = [&](const std::string& id) {
    for (const auto& module : harness.engine->enabledSet().modules)
      if (module.id == id)
        return module;
    return ModuleEnabled{};
  };
  const auto consistent = [&] {
    for (const auto& module : harness.engine->enabledSet().modules)
      if (module.enabled != (module.lifecycle == "active"))
        return false;
    return true;
  };
  CHECK(lifecycleOf("surveillance").enabled);
  CHECK(lifecycleOf("surveillance").lifecycle == "active");
  CHECK(lifecycleOf("core").lifecycle == "active");
  CHECK(consistent());

  static_cast<void>(harness.engine->disable({.moduleId = "surveillance", .userId = 1}));
  CHECK_FALSE(lifecycleOf("surveillance").enabled);
  CHECK(lifecycleOf("surveillance").lifecycle == "disabled");
  CHECK(consistent());

  static_cast<void>(harness.engine->uninstall({.moduleId = "surveillance", .userId = 1, .keepData = true, .pin = {}}));
  harness.step(2);
  CHECK_FALSE(lifecycleOf("surveillance").enabled);
  CHECK(lifecycleOf("surveillance").lifecycle == "uninstalled_data_kept");
  CHECK(consistent());
}

TEST_CASE("the engine's module actions reach the activity journal with who did them")
{
  Harness harness;
  ActionSink actions;
  ModuleJournal journal({.db = drogon::orm::DbClient::newSqlite3Client("filename=" + harness.path.string(), 1),
                         .sink = &actions,
                         .pollInterval = std::chrono::milliseconds(50)});
  harness.step();
  static_cast<void>(harness.engine->disable({.moduleId = "productivity", .userId = 5}));
  harness.step();
  CHECK(journal.relay() > 0);

  std::vector<ModuleActionRecord> disabled;
  for (const auto& record : actions.records)
    if (record.event.module == "productivity" && record.event.newData["event"].asString() == "disabled")
      disabled.push_back(record);
  REQUIRE(disabled.size() == 1);
  CHECK(disabled.front().event.userId == 5);
  CHECK(disabled.front().event.subject == "module");
  CHECK(disabled.front().event.newData["lifecycle"].asString() == "disabled");
  CHECK(disabled.front().event.action == UserAction::Update);

  const auto adopted = std::ranges::count_if(actions.records, [](const ModuleActionRecord& record) {
    return record.event.newData["event"].asString() == "adopted";
  });
  CHECK(adopted > 0);
}

namespace
{
ImpactRoleHolder holder(std::int64_t id, const std::string& role)
{
  return {.userId = id, .name = "Gus" + std::to_string(id), .lastName = "", .role = role, .isActive = true};
}

std::string refusalCode(const ModuleImpactView& view)
{
  return view.refusal ? std::string(view.refusal->wireCode()) : std::string();
}

void surveillanceOn(Harness& harness)
{
  harness.owners.set("vlm", installed("vision"));
  harness.step();
  REQUIRE(harness.enabled("surveillance"));
}

void describeImpact(Harness& harness)
{
  harness.owners.impacts["identity"] = {.reach = OwnerReach::Answered,
                                        .report = {.stops = {},
                                                   .roleHolders = {holder(7, "guard"), holder(8, "guard")},
                                                   .invitations = {{.id = 3,
                                                                    .role = "guard",
                                                                    .createdBy = 1,
                                                                    .createdByName = "Olga",
                                                                    .expiresAt = 5000}}}};
  harness.owners.impacts["guard"] = {.reach = OwnerReach::Answered,
                                     .report = {.stops = {{.kind = "pending_alerts", .count = 3}, {.kind = "guard_duty", .count = 1}},
                                                .roleHolders = {},
                                                .invitations = {}}};
  harness.owners.impacts["camera"] = {.reach = OwnerReach::Answered,
                                      .report = {.stops = {{.kind = "camera_talk", .count = 2}},
                                                 .roleHolders = {},
                                                 .invitations = {}}};
}
}

TEST_CASE("the impact of a module lists what stops, who holds its role, its invitations and what keeps running")
{
  Harness harness;
  surveillanceOn(harness);
  describeImpact(harness);

  const auto disable = harness.engine->impact({.moduleId = "surveillance", .action = ImpactAction::Disable});
  CHECK_FALSE(disable.refusal.has_value());
  REQUIRE(disable.stops.size() == 4);
  CHECK(disable.stops[0].kind == "live_views");
  CHECK_FALSE(disable.stops[0].count.has_value());
  CHECK(disable.stops[1].kind == "camera_talk");
  CHECK(disable.stops[1].count == 2);
  CHECK(disable.stops[2].kind == "guard_duty");
  CHECK(disable.stops[2].count == 1);
  CHECK(disable.stops[3].kind == "pending_alerts");
  CHECK(disable.stops[3].count == 3);
  REQUIRE(disable.roleHolders.size() == 2);
  CHECK(disable.roleHolders[0].userId == 7);
  CHECK(disable.roleEffect == "inactive");
  CHECK(disable.reassignRoles.empty());
  REQUIRE(disable.invitations.size() == 1);
  CHECK(disable.invitations[0].createdByName == "Olga");
  REQUIRE(disable.keepsRunning.size() == 1);
  CHECK(disable.keepsRunning[0].id == "safety_alerts");
  CHECK(disable.keepsRunning[0].text.es == "Las alertas de pánico seguirán.");
  CHECK(disable.filesBytes == 2000);
  CHECK(disable.data.size() == 2);
  CHECK(disable.unreachable.empty());

  const auto uninstall = harness.engine->impact({.moduleId = "surveillance", .action = ImpactAction::Uninstall});
  CHECK(uninstall.roleEffect == "reassign_required");
  CHECK(uninstall.reassignRoles == std::vector<std::string>{"resident", "guest"});

  harness.owners.impacts["identity"].report.roleHolders.clear();
  CHECK(harness.engine->impact({.moduleId = "surveillance", .action = ImpactAction::Uninstall}).roleEffect == "none");
  CHECK(harness.engine->impact({.moduleId = "productivity", .action = ImpactAction::Disable}).stops.empty());
}

TEST_CASE("the impact refuses what the action would refuse and names the owner that did not answer")
{
  Harness harness;
  surveillanceOn(harness);
  describeImpact(harness);

  const auto core = harness.engine->impact({.moduleId = "core", .action = ImpactAction::Disable});
  REQUIRE(core.refusal.has_value());
  CHECK(refusalCode(core) == "MODULE_CORE");

  harness.owners.set("llm", installed("insight"));
  static_cast<void>(harness.engine->install({.moduleId = "insights", .userId = 1}));
  harness.step();
  REQUIRE(harness.enabled("insights"));
  const auto required = harness.engine->impact({.moduleId = "surveillance", .action = ImpactAction::Uninstall});
  REQUIRE(required.refusal.has_value());
  CHECK(refusalCode(required) == "MODULE_REQUIRED_BY");

  CHECK(statusOf([&] { static_cast<void>(harness.engine->impact({.moduleId = "ghost", .action = ImpactAction::Disable})); }) == 404);

  harness.owners.impacts["identity"].reach = OwnerReach::Unreachable;
  const auto blind = harness.engine->impact({.moduleId = "surveillance", .action = ImpactAction::Disable});
  CHECK(blind.unreachable == std::vector<std::string>{"identity"});
  CHECK(blind.roleHolders.empty());
}

TEST_CASE("uninstalling a module whose role people hold is refused with the holders unless each gets a role of an active module")
{
  Harness harness;
  surveillanceOn(harness);
  describeImpact(harness);
  harness.owners.reassignReply = {.reach = OwnerReach::Answered,
                                  .value = RoleReassignmentOutcome{.status = ReassignStatus::Applied,
                                                                   .applied = 2,
                                                                   .failedUserId = 0,
                                                                   .reason = {}}};
  const auto uninstall = [&](std::vector<RoleReassignment> reassign) {
    return harness.engine->uninstall(
        {.moduleId = "surveillance", .userId = 1, .keepData = true, .pin = {}, .reassign = std::move(reassign)});
  };

  try {
    static_cast<void>(uninstall({}));
    FAIL("the uninstall must be refused while two people hold the guard role");
  }
  catch (const ResponseException& error) {
    CHECK(error.statusCode() == 409);
    CHECK(error.errorCode() == "MODULE_ROLES_HELD");
    const auto* list = std::get_if<std::vector<ResponseError>>(&error.errors());
    REQUIRE(list != nullptr);
    REQUIRE(list->size() == 3);
    CHECK((*list)[1].code == "ROLE_HOLDER");
    CHECK((*list)[1].message == "7:guard");
    CHECK((*list)[2].message == "8:guard");
  }
  CHECK(harness.enabled("surveillance"));
  CHECK_FALSE(harness.view("surveillance").job.has_value());
  CHECK(harness.owners.callsOf("reassign:").empty());

  CHECK(codeOf([&] { static_cast<void>(uninstall({{.userId = 7, .role = "resident"}})); }) == "MODULE_ROLES_HELD");
  CHECK_THROWS_AS(static_cast<void>(uninstall({{.userId = 7, .role = "guard"}, {.userId = 8, .role = "resident"}})),
                  ValidationException);
  CHECK(harness.owners.callsOf("reassign:").empty());

  harness.owners.reassignReply = {.reach = OwnerReach::Answered,
                                  .value = RoleReassignmentOutcome{.status = ReassignStatus::Refused,
                                                                   .applied = 1,
                                                                   .failedUserId = 8,
                                                                   .reason = "user_inactive"}};
  CHECK(codeOf([&] { static_cast<void>(uninstall({{.userId = 7, .role = "resident"}, {.userId = 8, .role = "guest"}})); }) ==
        "CONFLICT");
  CHECK_FALSE(harness.view("surveillance").job.has_value());

  harness.owners.reassignReply = {.reach = OwnerReach::Unreachable, .value = std::nullopt};
  CHECK(statusOf([&] { static_cast<void>(uninstall({{.userId = 7, .role = "resident"}, {.userId = 8, .role = "guest"}})); }) ==
        503);
  CHECK_FALSE(harness.view("surveillance").job.has_value());

  harness.owners.reassignReply = {.reach = OwnerReach::Answered,
                                  .value = RoleReassignmentOutcome{.status = ReassignStatus::Applied,
                                                                   .applied = 2,
                                                                   .failedUserId = 0,
                                                                   .reason = {}}};
  const auto job = uninstall({{.userId = 7, .role = "resident"}, {.userId = 8, .role = "guest"}, {.userId = 99, .role = "guest"}});
  CHECK(job.job.kind == JobKind::Uninstall);
  CHECK(harness.owners.lastBatch.actorUserId == 1);
  REQUIRE(harness.owners.lastBatch.reassignments.size() == 2);
  CHECK(harness.owners.lastBatch.reassignments[0].userId == 7);
  CHECK(harness.owners.lastBatch.reassignments[0].role == "resident");
  CHECK(harness.owners.lastBatch.reassignments[1].role == "guest");
}

TEST_CASE("a wrong PIN is refused before any role is reassigned, and a module nobody holds a role of needs no reassignment")
{
  Harness harness;
  surveillanceOn(harness);
  describeImpact(harness);
  harness.owners.pin = {.reach = OwnerReach::Answered, .value = PinVerdict::Invalid};
  CHECK(codeOf([&] {
          static_cast<void>(harness.engine->uninstall({.moduleId = "surveillance",
                                                       .userId = 1,
                                                       .keepData = false,
                                                       .pin = "0000",
                                                       .reassign = {{.userId = 7, .role = "resident"}, {.userId = 8, .role = "guest"}}}));
        }) == "PIN_INVALID");
  CHECK(harness.owners.callsOf("reassign:").empty());

  harness.owners.impacts["identity"].report.roleHolders.clear();
  harness.owners.pin = {.reach = OwnerReach::Answered, .value = PinVerdict::Accepted};
  const auto job = harness.engine->uninstall(
      {.moduleId = "surveillance", .userId = 1, .keepData = false, .pin = "2468"});
  CHECK(job.job.kind == JobKind::Purge);
  CHECK(harness.owners.callsOf("reassign:").empty());
}

TEST_CASE("an identity that cannot be asked blocks the uninstall of a role-bearing module instead of guessing")
{
  Harness harness;
  surveillanceOn(harness);
  harness.owners.impacts["identity"] = {.reach = OwnerReach::Unreachable, .report = {}};
  CHECK(statusOf([&] {
          static_cast<void>(harness.engine->uninstall({.moduleId = "surveillance", .userId = 1, .keepData = true, .pin = {}}));
        }) == 503);
  CHECK(harness.enabled("surveillance"));
}

TEST_CASE("a request names the module to the notification owner and refuses what needs none")
{
  Harness harness;
  harness.step();
  harness.owners.requestReply = {.reach = OwnerReach::Answered,
                                 .value = ModuleRequestOutcome{.notified = 1, .duplicate = false}};
  const auto ask = [&](UserRole role, const std::string& id = "insights") {
    return harness.engine->request({.moduleId = id, .userId = 5, .role = role});
  };

  const auto first = ask(UserRole::Resident);
  CHECK(first.moduleId == "insights");
  CHECK_FALSE(first.duplicate);
  CHECK(harness.owners.lastRequest.userId == 5);
  CHECK(harness.owners.lastRequest.moduleName.en == "insights");
  CHECK(harness.owners.lastRequest.day.size() == 10);
  CHECK(harness.owners.lastRequest.day[4] == '-');
  CHECK(harness.owners.callsOf("request:notification:insights:5").size() == 1);

  harness.owners.requestReply = {.reach = OwnerReach::Answered,
                                 .value = ModuleRequestOutcome{.notified = 0, .duplicate = true}};
  CHECK(ask(UserRole::Guard).duplicate);

  CHECK(codeOf([&] { static_cast<void>(ask(UserRole::Owner)); }) == "CONFLICT");
  CHECK(codeOf([&] { static_cast<void>(ask(UserRole::Resident, "core")); }) == "CONFLICT");
  CHECK(codeOf([&] { static_cast<void>(ask(UserRole::Resident, "agronomy")); }) == "MODULE_COMING_SOON");
  CHECK(statusOf([&] { static_cast<void>(ask(UserRole::Resident, "ghost")); }) == 404);

  harness.owners.requestReply = {.reach = OwnerReach::Unreachable, .value = std::nullopt};
  CHECK(statusOf([&] { static_cast<void>(ask(UserRole::Resident)); }) == 503);
  harness.owners.requestReply = {.reach = OwnerReach::Unsupported, .value = std::nullopt};
  CHECK(statusOf([&] { static_cast<void>(ask(UserRole::Resident)); }) == 503);

  harness.owners.requestReply = {.reach = OwnerReach::Answered,
                                 .value = ModuleRequestOutcome{.notified = 1, .duplicate = false}};
  harness.owners.set("llm", installed("insight"));
  harness.owners.set("vlm", installed("vision"));
  harness.step();
  static_cast<void>(harness.engine->install({.moduleId = "insights", .userId = 1}));
  CHECK(codeOf([&] { static_cast<void>(ask(UserRole::Resident)); }) == "MODULE_JOB_RUNNING");
}

TEST_CASE("the settings of the owners a module brings are visible only while the module is on")
{
  Harness harness;
  harness.owners.set("vlm", installed("vision"));
  harness.step();
  REQUIRE(harness.enabled("surveillance"));
  CHECK(harness.engine->settingsOwnerVisible("camera"));
  CHECK(harness.engine->settingsOwnerVisible("guard"));
  CHECK(harness.engine->settingsOwnerVisible("llm"));

  static_cast<void>(harness.engine->disable({.moduleId = "surveillance", .userId = 1}));
  CHECK_FALSE(harness.engine->settingsOwnerVisible("camera"));
  CHECK_FALSE(harness.engine->settingsOwnerVisible("guard"));
  CHECK(harness.engine->settingsOwnerVisible("llm"));
  CHECK(harness.engine->settingsOwnerVisible("tts"));

  harness.step();
  harness.owners.set("vlm", installed("vision"));
  static_cast<void>(harness.engine->install({.moduleId = "surveillance", .userId = 1}));
  harness.step(3);
  CHECK(harness.enabled("surveillance"));
  CHECK(harness.engine->settingsOwnerVisible("camera"));
}
