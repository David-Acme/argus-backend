#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/config-service.hxx>
#include <errors/response-exception.hxx>
#include <settings/component-host.hxx>
#include <settings/module-data-host.hxx>
#include <settings/owner-pin-host.hxx>
#include <settings/settings-client.hxx>
#include <settings/settings-rpc.hxx>

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

namespace
{
constexpr const char* kSecret = "settings-client-secret";

std::filesystem::path configPath()
{
  return std::filesystem::temp_directory_path() / "argus-settings-client-test.toml";
}

void loadConfig(const std::string& content)
{
  std::ofstream(configPath()) << content;
  ConfigService::load(configPath().string());
}

std::vector<SettingSpec> catalogSpecs()
{
  return {{.key = "tts.speed",
           .group = "voice",
           .type = SettingType::Decimal,
           .level = SettingLevel::Basic,
           .apply = SettingApply::Live,
           .range = {.min = 0.7, .max = 2.0, .step = 0.05},
           .choices = {},
           .fallback = "1",
           .unit = "x"},
          {.key = "tts.quality",
           .group = "voice",
           .type = SettingType::Choice,
           .level = SettingLevel::Advanced,
           .apply = SettingApply::NextSession,
           .range = {},
           .choices = {"auto", "low", "high"},
           .fallback = "auto"}};
}

struct Owner
{
  SettingsRegistry registry{catalogSpecs()};
  SettingsRpcService service{{.service = "tts",
                              .registry = &registry,
                              .credentials = {{.service = "settings", .secret = kSecret}}}};
  std::unique_ptr<grpc::Server> server;
  int port{0};

  Owner()
  {
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    server = builder.BuildAndStart();
  }

  ~Owner() { server->Shutdown(); }

  Owner(const Owner&) = delete;
  Owner& operator=(const Owner&) = delete;

  [[nodiscard]] std::string target() const { return "127.0.0.1:" + std::to_string(port); }
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
}

TEST_CASE("the constructor refuses a configuration it cannot dial")
{
  const auto build = [](const SettingsClientConfig& config) {
    return [config] { const SettingsClient client(config); };
  };
  CHECK(statusOf(build({.target = "", .credential = kSecret, .timeout = std::chrono::seconds(1)})) == 400);
  CHECK(statusOf(build({.target = "127.0.0.1:1", .credential = "", .timeout = std::chrono::seconds(1)})) == 400);
  CHECK(statusOf(build({.target = "127.0.0.1:1", .credential = kSecret, .timeout = std::chrono::milliseconds(0)})) ==
        400);
  CHECK(statusOf(build({.target = "127.0.0.1:1", .credential = kSecret, .timeout = std::chrono::seconds(121)})) ==
        400);
  CHECK(statusOf(build({.target = "127.0.0.1:1", .credential = kSecret, .timeout = std::chrono::seconds(120)})) == 0);
}

TEST_CASE("list reads the owner's catalog into the shared vocabulary")
{
  loadConfig("[tts]\nspeed = 1.25\n");
  const Owner owner;
  const SettingsClient client({.target = owner.target(), .credential = kSecret, .timeout = std::chrono::seconds(5)});

  const auto catalog = client.list();
  CHECK(catalog.service == "tts");
  REQUIRE(catalog.settings.size() == 2);
  const auto& speed = catalog.settings[0];
  CHECK(speed.spec.key == "tts.speed");
  CHECK(speed.spec.group == "voice");
  CHECK(speed.spec.type == SettingType::Decimal);
  CHECK(speed.spec.level == SettingLevel::Basic);
  CHECK(speed.spec.apply == SettingApply::Live);
  CHECK(speed.spec.range.min == doctest::Approx(0.7));
  CHECK(speed.spec.range.max == doctest::Approx(2.0));
  CHECK(speed.spec.range.step == doctest::Approx(0.05));
  CHECK(speed.spec.fallback == "1");
  CHECK(speed.value == "1.25");
  const auto& quality = catalog.settings[1];
  CHECK(quality.spec.type == SettingType::Choice);
  CHECK(quality.spec.apply == SettingApply::NextSession);
  CHECK(quality.spec.choices == std::vector<std::string>{"auto", "low", "high"});
  CHECK(quality.value == "auto");
  std::filesystem::remove(configPath());
}

TEST_CASE("update reports what was applied, what was refused and the fresh catalog")
{
  loadConfig("[tts]\nspeed = 1.0\n");
  const Owner owner;
  const SettingsClient client({.target = owner.target(), .credential = kSecret, .timeout = std::chrono::seconds(5)});

  const auto applied = client.update({{.key = "tts.speed", .value = "1.5"}});
  CHECK(applied.applied == std::vector<std::string>{"tts.speed"});
  CHECK(applied.rejected.empty());
  CHECK(applied.catalog.settings[0].value == "1.5");

  const auto refused =
      client.update({{.key = "tts.speed", .value = "9"}, {.key = "tts.quality", .value = "ultra"},
                     {.key = "tts.missing", .value = "1"}});
  CHECK(refused.applied.empty());
  REQUIRE(refused.rejected.size() == 3);
  CHECK(refused.rejected[0].key == "tts.speed");
  CHECK(refused.rejected[0].reason == SettingRejectionReason::OutOfRange);
  CHECK(refused.rejected[1].reason == SettingRejectionReason::NotAChoice);
  CHECK(refused.rejected[2].reason == SettingRejectionReason::Unknown);
  CHECK(refused.catalog.settings[0].value == "1.5");
  std::filesystem::remove(configPath());
}

TEST_CASE("a refused credential, an empty update and a dead owner surface as response refusals")
{
  loadConfig("[tts]\nspeed = 1.0\n");
  const Owner owner;

  const SettingsClient stranger({.target = owner.target(), .credential = "not-the-secret",
                                 .timeout = std::chrono::seconds(5)});
  CHECK(statusOf([&stranger] { (void)stranger.list(); }) == 401);

  const SettingsClient client({.target = owner.target(), .credential = kSecret, .timeout = std::chrono::seconds(5)});
  CHECK(statusOf([&client] { (void)client.update({}); }) == 400);

  const SettingsClient dead({.target = "127.0.0.1:1", .credential = kSecret,
                             .timeout = std::chrono::milliseconds(500)});
  const int status = statusOf([&dead] { (void)dead.list(); });
  CHECK((status == 503 || status == 504));
  std::filesystem::remove(configPath());
}

TEST_CASE("the installation state of each choice and a not-installed refusal come back through the client")
{
  loadConfig("[tts]\nquality = \"auto\"\n");
  Owner owner;
  owner.registry.describeChoices([](const SettingSpec& spec) {
    if (spec.key != "tts.quality")
      return std::vector<ChoiceState>{};
    return std::vector<ChoiceState>{
        {.choice = "auto", .availability = ChoiceAvailability::Installed, .sizeMb = 0, .hostCommand = ""},
        {.choice = "low", .availability = ChoiceAvailability::Installing, .sizeMb = 5.8, .hostCommand = "provision low"},
        {.choice = "high", .availability = ChoiceAvailability::HostOnly, .sizeMb = 24.7, .hostCommand = "provision high"}};
  });
  const SettingsClient client({.target = owner.target(), .credential = kSecret, .timeout = std::chrono::seconds(5)});

  const auto catalog = client.list();
  REQUIRE(catalog.settings.size() == 2);
  CHECK(catalog.settings[0].choiceStates.empty());
  const auto& states = catalog.settings[1].choiceStates;
  REQUIRE(states.size() == 3);
  CHECK(states[0].availability == ChoiceAvailability::Installed);
  CHECK(states[1].choice == "low");
  CHECK(states[1].availability == ChoiceAvailability::Installing);
  CHECK(states[1].sizeMb == doctest::Approx(5.8));
  CHECK(states[1].hostCommand == "provision low");
  CHECK(states[2].availability == ChoiceAvailability::HostOnly);

  const auto refused = client.update({{.key = "tts.quality", .value = "high"}});
  REQUIRE(refused.rejected.size() == 1);
  CHECK(refused.rejected[0].reason == SettingRejectionReason::NotInstalled);
  CHECK(refused.catalog.settings[1].value == "auto");
  std::filesystem::remove(configPath());
}

TEST_CASE("the catalog carries units, the config path, capabilities and a profile marker an update can record")
{
  loadConfig("[tts]\nspeed = 1.0\n");
  Owner owner;
  owner.registry.declareCapability("gpu");
  const SettingsClient client({.target = owner.target(), .credential = kSecret, .timeout = std::chrono::seconds(5)});

  const auto before = client.list();
  CHECK(before.settings[0].spec.unit == "x");
  CHECK(before.settings[1].spec.unit.empty());
  CHECK(before.configPath == std::filesystem::absolute(configPath()).lexically_normal().string());
  CHECK(before.capabilities == std::vector<std::string>{"gpu"});
  if (!before.profile) {
    FAIL("expected a value in before.profile");
    return;
  }
  CHECK(before.profile->origin == ProfileOrigin::None);
  CHECK(before.profile->id.empty());

  const auto reply = client.update({{.key = "tts.speed", .value = "1.2"}},
                                   ProfileMarker{.id = "balanced",
                                                 .origin = ProfileOrigin::Recommended,
                                                 .appliedAt = 1759500000,
                                                 .keys = {"tts.speed"}});
  CHECK(reply.applied == std::vector<std::string>{"tts.speed"});
  CHECK(reply.profileRecorded);
  if (!reply.catalog.profile) {
    FAIL("expected a value in reply.catalog.profile");
    return;
  }
  CHECK(reply.catalog.profile->id == "balanced");
  CHECK(reply.catalog.profile->origin == ProfileOrigin::Recommended);
  CHECK(reply.catalog.profile->appliedAt == 1759500000);
  CHECK(reply.catalog.profile->keys == std::vector<std::string>{"tts.speed"});

  const auto markOnly = client.update({}, ProfileMarker{.id = "balanced",
                                                        .origin = ProfileOrigin::Reverted,
                                                        .appliedAt = 1759500100,
                                                        .keys = {}});
  CHECK(markOnly.applied.empty());
  CHECK(markOnly.profileRecorded);
  const auto reverted = client.list();
  if (!reverted.profile) {
    FAIL("the reverted catalog carries no profile");
    return;
  }
  CHECK(reverted.profile->origin == ProfileOrigin::Reverted);

  const auto refused = client.update({{.key = "tts.speed", .value = "9"}},
                                     ProfileMarker{.id = "quality",
                                                   .origin = ProfileOrigin::Owner,
                                                   .appliedAt = 1759500200,
                                                   .keys = {}});
  CHECK_FALSE(refused.profileRecorded);
  const auto kept = client.list();
  if (!kept.profile) {
    FAIL("the catalog lost its profile after a refused write");
    return;
  }
  CHECK(kept.profile->id == "balanced");
  std::filesystem::remove(configPath());
}

TEST_CASE("component calls read an owner's host and an owner without one answers nothing")
{
  loadConfig("[tts]\nspeed = 1.0\n");
  Owner owner;
  const SettingsClient client({.target = owner.target(), .credential = kSecret, .timeout = std::chrono::seconds(5)});
  const ComponentSpec spec{.id = "voice-tts",
                           .source = ComponentSource::Provisioned,
                           .files = {{.path = "tts/model.onnx", .url = {}, .sizeBytes = 3, .sha256 = {}}},
                           .hostCommand = "services/tts/scripts/provision.sh"};

  CHECK_FALSE(client.componentStates({spec}).has_value());
  CHECK_FALSE(client.installComponent(spec).has_value());

  const auto models = std::filesystem::temp_directory_path() / "argus-settings-client-components";
  std::filesystem::create_directories(models / "tts");
  std::ofstream(models / "tts/model.onnx") << "abc";
  DiskComponentHost host({.modelsDir = models, .owned = {"voice-tts"}, .fetch = {}, .ready = {}});
  owner.service.attachComponents(host);

  const auto states = client.componentStates({spec}).value_or(std::vector<ComponentStatus>{});
  REQUIRE(states.size() == 1);
  CHECK(states.front().state == ComponentState::Installed);
  CHECK(states.front().bytesPresent == 3);
  CHECK(states.front().ready);
  const auto cancelled = client.cancelComponent(spec);
  CHECK(cancelled.has_value());
  CHECK(cancelled.value_or(ComponentStatus{}).state == ComponentState::Installed);
  std::filesystem::remove_all(models);
}

namespace
{
class FakeModules final : public argus::settings::v1::Modules::CallbackService
{
public:
  grpc::ServerUnaryReactor* ModuleStates(grpc::CallbackServerContext* context,
                                         const argus::settings::v1::ModuleStatesRequest*,
                                         argus::settings::v1::ModuleStatesResponse* response) override
  {
    auto* reactor = context->DefaultReactor();
    if (argus::client::metadata(context, argus::client::kCallerCredentialKey) != kSecret) {
      reactor->Finish({grpc::StatusCode::UNAUTHENTICATED, "no"});
      return reactor;
    }
    auto* module = response->add_modules();
    module->set_id("surveillance");
    module->set_enabled(true);
    response->set_version(7);
    response->set_settled(true);
    reactor->Finish(grpc::Status::OK);
    return reactor;
  }
};
}

TEST_CASE("the modules client reads the enabled set")
{
  FakeModules service;
  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&service);
  const auto server = builder.BuildAndStart();
  const std::string target = "127.0.0.1:" + std::to_string(port);

  const ModulesClient client({.target = target, .credential = kSecret, .timeout = std::chrono::seconds(5)});
  const auto reply = client.moduleStates();
  REQUIRE(reply.modules.size() == 1);
  CHECK(reply.modules.front().id == "surveillance");
  CHECK(reply.modules.front().enabled);
  CHECK(reply.version == 7);
  CHECK(reply.settled);

  const ModulesClient stranger({.target = target, .credential = "wrong", .timeout = std::chrono::seconds(5)});
  CHECK(statusOf([&stranger] { static_cast<void>(stranger.moduleStates()); }) == 401);
  CHECK_THROWS(ModulesClient({.target = "", .credential = kSecret, .timeout = std::chrono::seconds(5)}));
  server->Shutdown();
}

namespace
{
class FakeModuleData final : public ModuleDataHost
{
public:
  [[nodiscard]] ModuleDataSummary summary(const std::string& moduleId) const override
  {
    if (moduleId != "surveillance")
      return {};
    return {.items = {{.kind = "camera", .count = 2}, {.kind = "event", .count = 40}}, .bytes = 1024};
  }

  ModuleDataPurge purge(const std::string& moduleId) override
  {
    purged = moduleId;
    return {.purged = true, .reason = {}};
  }

  std::string purged;
};
}

TEST_CASE("module data calls answer nothing without a host and report and purge with one")
{
  loadConfig("[tts]\nspeed = 1.0\n");
  Owner owner;
  const SettingsClient client({.target = owner.target(), .credential = kSecret, .timeout = std::chrono::seconds(5)});
  CHECK_FALSE(client.moduleDataSummary("surveillance").has_value());
  CHECK_FALSE(client.purgeModuleData("surveillance").has_value());

  FakeModuleData data;
  owner.service.attachModuleData(data);
  const auto reported = client.moduleDataSummary("surveillance");
  CHECK(reported.has_value());
  const auto summary = reported.value_or(ModuleDataSummary{});
  REQUIRE(summary.items.size() == 2);
  CHECK(summary.items[1].kind == "event");
  CHECK(summary.items[1].count == 40);
  CHECK(summary.bytes == 1024);
  CHECK_FALSE(summary.empty());
  const auto empty = client.moduleDataSummary("productivity");
  CHECK(empty.has_value());
  CHECK(empty.value_or(ModuleDataSummary{.items = {{.kind = "x", .count = 1}}, .bytes = 1}).empty());

  const auto purge = client.purgeModuleData("surveillance");
  CHECK(purge.value_or(ModuleDataPurge{}).purged);
  CHECK(data.purged == "surveillance");
  CHECK(statusOf([&client] { static_cast<void>(client.purgeModuleData("")); }) == 400);
}

namespace
{
class FakeOwnerPin final : public OwnerPinHost
{
public:
  PinVerdict verify(const OwnerPinCheck& check) override
  {
    if (check.pin.empty())
      return PinVerdict::Required;
    return check.pin == "2468" ? PinVerdict::Accepted : PinVerdict::Invalid;
  }
};
}

TEST_CASE("the owner PIN check answers nothing without a host and the guard's verdict with one")
{
  loadConfig("[tts]\nspeed = 1.0\n");
  Owner owner;
  const SettingsClient client({.target = owner.target(), .credential = kSecret, .timeout = std::chrono::seconds(5)});
  CHECK_FALSE(client.verifyOwnerPin(1, "2468").has_value());

  FakeOwnerPin pin;
  owner.service.attachOwnerPin(pin);
  CHECK(client.verifyOwnerPin(1, "2468") == PinVerdict::Accepted);
  CHECK(client.verifyOwnerPin(1, "0000") == PinVerdict::Invalid);
  CHECK(client.verifyOwnerPin(1, "") == PinVerdict::Required);
  CHECK(statusOf([&client] { static_cast<void>(client.verifyOwnerPin(0, "2468")); }) == 400);
}
