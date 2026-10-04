#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/config-service.hxx>
#include <errors/response-exception.hxx>
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
