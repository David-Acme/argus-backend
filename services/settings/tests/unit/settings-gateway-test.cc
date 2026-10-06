#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/config-service.hxx>
#include <errors/response-exception.hxx>
#include <errors/validation-exception.hxx>
#include <feature/settings/services/owner-visibility.hxx>
#include <feature/settings/services/settings-gateway-service.hxx>
#include <settings/settings-rpc.hxx>

#include <grpcpp/grpcpp.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace
{
namespace wire = argus::settings::v1;

using namespace std::chrono_literals;

constexpr const char* kSecret = "gateway-secret";

std::filesystem::path configPath()
{
  return std::filesystem::temp_directory_path() / "argus-settings-gateway-test.toml";
}

void loadConfig()
{
  std::ofstream(configPath()) << "[tts]\nspeed = 1.25\n\n[voice]\nlanguage = \"es\"\n";
  ConfigService::load(configPath().string());
}

std::vector<SettingSpec> ttsSpecs()
{
  return {{.key = "tts.speed",
           .group = "voice",
           .type = SettingType::Decimal,
           .level = SettingLevel::Basic,
           .apply = SettingApply::Live,
           .range = {.min = 0.7, .max = 2.0, .step = 0.05},
           .choices = {},
           .fallback = "1"}};
}

std::vector<SettingSpec> voiceSpecs()
{
  return {{.key = "voice.language",
           .group = "call",
           .type = SettingType::Choice,
           .level = SettingLevel::Basic,
           .apply = SettingApply::NextSession,
           .range = {},
           .choices = {"es", "en"},
           .fallback = "es"}};
}

struct Owner
{
  SettingsRegistry registry;
  SettingsRpcService service;
  std::unique_ptr<grpc::Server> server;
  int port{0};

  Owner(const std::string& name, std::vector<SettingSpec> specs)
      : registry(std::move(specs)),
        service({.service = name, .registry = &registry, .credentials = {{.service = "settings", .secret = kSecret}}})
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

class StallingSettings final : public wire::Settings::Service
{
public:
  grpc::Status List(grpc::ServerContext*, const wire::ListSettingsRequest*, wire::SettingsCatalog*) override
  {
    std::this_thread::sleep_for(2500ms);
    return grpc::Status::OK;
  }

  grpc::Status Update(grpc::ServerContext*, const wire::UpdateSettingsRequest*,
                      wire::UpdateSettingsResponse*) override
  {
    std::this_thread::sleep_for(2500ms);
    return grpc::Status::OK;
  }
};

struct StallingOwner
{
  StallingSettings service;
  std::unique_ptr<grpc::Server> server;
  int port{0};

  StallingOwner()
  {
    grpc::ServerBuilder builder;
    builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
    builder.RegisterService(&service);
    server = builder.BuildAndStart();
  }

  ~StallingOwner() { server->Shutdown(); }

  StallingOwner(const StallingOwner&) = delete;
  StallingOwner& operator=(const StallingOwner&) = delete;

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

ValidationErrors rejectionsOf(const std::function<void()>& call)
{
  try {
    call();
  }
  catch (const ValidationException& error) {
    return error.errors();
  }
  return {};
}
}

TEST_CASE("the catalogs keep the configured order and a dead owner is unreachable, not an error")
{
  loadConfig();
  const Owner tts("tts", ttsSpecs());
  const Owner voice("voice", voiceSpecs());
  const SettingsGatewayService gateway(
      {.owners = {{.name = "voice", .target = voice.target(), .credential = kSecret},
                  {.name = "tts", .target = tts.target(), .credential = kSecret},
                  {.name = "stt", .target = "127.0.0.1:1", .credential = kSecret},
                  {.name = "vlm", .target = tts.target(), .credential = "not-the-secret"}},
       .timeouts = {.list = 1500ms, .update = 5000ms}});

  const auto catalogs = gateway.catalogs();
  REQUIRE(catalogs.size() == 4);
  CHECK(catalogs[0].service == "voice");
  CHECK(catalogs[0].reachable);
  REQUIRE(catalogs[0].settings.size() == 1);
  CHECK(catalogs[0].settings[0].spec.key == "voice.language");
  CHECK(catalogs[0].settings[0].value == "es");
  CHECK(catalogs[1].service == "tts");
  CHECK(catalogs[1].reachable);
  CHECK(catalogs[1].settings[0].value == "1.25");
  CHECK(catalogs[2].service == "stt");
  CHECK_FALSE(catalogs[2].reachable);
  CHECK(catalogs[2].settings.empty());
  CHECK(catalogs[3].service == "vlm");
  CHECK_FALSE(catalogs[3].reachable);
  std::filesystem::remove(configPath());
}

TEST_CASE("owners that hang are queried in parallel, each under its own deadline")
{
  loadConfig();
  const StallingOwner first;
  const StallingOwner second;
  const Owner tts("tts", ttsSpecs());
  const SettingsGatewayService gateway(
      {.owners = {{.name = "llm", .target = first.target(), .credential = kSecret},
                  {.name = "voice", .target = second.target(), .credential = kSecret},
                  {.name = "tts", .target = tts.target(), .credential = kSecret}},
       .timeouts = {.list = 1000ms, .update = 1000ms}});

  const auto started = std::chrono::steady_clock::now();
  const auto catalogs = gateway.catalogs();
  const auto elapsed = std::chrono::steady_clock::now() - started;
  CHECK(elapsed < 1900ms);
  REQUIRE(catalogs.size() == 3);
  CHECK_FALSE(catalogs[0].reachable);
  CHECK_FALSE(catalogs[1].reachable);
  CHECK(catalogs[2].reachable);

  CHECK(statusOf([&gateway] { (void)gateway.update({.owner = "llm", .changes = {{.key = "llm.x", .value = "1"}}, .userId = 1}); }) ==
        503);
  std::filesystem::remove(configPath());
}

TEST_CASE("an update reaches only a configured owner and answers with the fresh catalog")
{
  loadConfig();
  const Owner tts("tts", ttsSpecs());
  const SettingsGatewayService gateway(
      {.owners = {{.name = "tts", .target = tts.target(), .credential = kSecret},
                  {.name = "stt", .target = "127.0.0.1:1", .credential = kSecret}},
       .timeouts = {.list = 1500ms, .update = 1500ms}});

  CHECK(statusOf([&gateway] {
          (void)gateway.update({.owner = "camera", .changes = {{.key = "tts.speed", .value = "1"}}, .userId = 1});
        }) == 404);
  CHECK(statusOf([&gateway] {
          (void)gateway.update({.owner = "stt", .changes = {{.key = "stt.language", .value = "es"}}, .userId = 1});
        }) == 503);

  const auto outcome = gateway.update({.owner = "tts", .changes = {{.key = "tts.speed", .value = "1.5"}}, .userId = 1});
  CHECK(outcome.applied == std::vector<std::string>{"tts.speed"});
  CHECK(outcome.catalog.service == "tts");
  CHECK(outcome.catalog.reachable);
  REQUIRE(outcome.catalog.settings.size() == 1);
  CHECK(outcome.catalog.settings[0].value == "1.5");
  CHECK(ConfigService::getDouble("tts.speed") == doctest::Approx(1.5));
  std::filesystem::remove(configPath());
}

TEST_CASE("a refused change is a validation error per key and applies nothing")
{
  loadConfig();
  const Owner tts("tts", ttsSpecs());
  const Owner voice("voice", voiceSpecs());
  const SettingsGatewayService gateway(
      {.owners = {{.name = "tts", .target = tts.target(), .credential = kSecret},
                  {.name = "voice", .target = voice.target(), .credential = kSecret}},
       .timeouts = {.list = 1500ms, .update = 1500ms}});

  const auto ttsErrors = rejectionsOf([&gateway] {
    (void)gateway.update({.owner = "tts",
                          .changes = {{.key = "tts.speed", .value = "9"},
                                      {.key = "tts.nope", .value = "1"},
                                      {.key = "tts.speed", .value = "fast"}},
                          .userId = 1});
  });
  REQUIRE(ttsErrors.size() == 2);
  CHECK(ttsErrors.at("tts.speed") == std::vector<std::string>{"outOfRange", "invalid"});
  CHECK(ttsErrors.at("tts.nope") == std::vector<std::string>{"unknownKey"});
  CHECK(ConfigService::getDouble("tts.speed") == doctest::Approx(1.25));

  const auto voiceErrors = rejectionsOf([&gateway] {
    (void)gateway.update({.owner = "voice", .changes = {{.key = "voice.language", .value = "fr"}}, .userId = 1});
  });
  CHECK(voiceErrors.at("voice.language") == std::vector<std::string>{"notAChoice"});
  std::filesystem::remove(configPath());
}

TEST_CASE("an owner that cannot persist a change is a server failure, not a validation error")
{
  loadConfig();
  const Owner tts("tts", ttsSpecs());
  const SettingsGatewayService gateway({.owners = {{.name = "tts", .target = tts.target(), .credential = kSecret}},
                                        .timeouts = {.list = 1500ms, .update = 1500ms}});
  std::filesystem::remove(configPath());

  CHECK(statusOf([&gateway] {
          (void)gateway.update({.owner = "tts", .changes = {{.key = "tts.speed", .value = "1.5"}}, .userId = 1});
        }) == 500);
}

TEST_CASE("choice states pass through to the app, and a host-only choice is refused as not installed")
{
  loadConfig();
  Owner voice("voice", voiceSpecs());
  voice.registry.describeChoices([](const SettingSpec&) {
    return std::vector<ChoiceState>{
        {.choice = "es", .availability = ChoiceAvailability::Installed, .sizeMb = 0, .hostCommand = ""},
        {.choice = "en", .availability = ChoiceAvailability::HostOnly, .sizeMb = 6.2, .hostCommand = "provision en"}};
  });
  const SettingsGatewayService gateway({.owners = {{.name = "voice", .target = voice.target(), .credential = kSecret}},
                                        .timeouts = {.list = 1500ms, .update = 1500ms}});

  const auto catalogs = gateway.catalogs();
  REQUIRE(catalogs.size() == 1);
  REQUIRE(catalogs[0].settings.size() == 1);
  const auto& states = catalogs[0].settings[0].choiceStates;
  REQUIRE(states.size() == 2);
  CHECK(states[1].availability == ChoiceAvailability::HostOnly);
  CHECK(states[1].hostCommand == "provision en");

  const auto errors = rejectionsOf([&gateway] {
    (void)gateway.update({.owner = "voice", .changes = {{.key = "voice.language", .value = "en"}}, .userId = 1});
  });
  CHECK(errors.at("voice.language") == std::vector<std::string>{"notInstalled"});
  CHECK(ConfigService::getString("voice.language") == "es");
  std::filesystem::remove(configPath());
}

TEST_CASE("unconfigured owners are listed in display order as not configured, beside the live ones")
{
  loadConfig();
  const Owner tts("tts", ttsSpecs());
  const SettingsGatewayService gateway({.owners = {{.name = "tts", .target = tts.target(), .credential = kSecret,
                                                    .configFile = "/srv/argus/config.tts.toml"}},
                                        .timeouts = {.list = 1500ms, .update = 1500ms},
                                        .unconfigured = {"vlm", "llm"}});
  const auto catalogs = gateway.catalogs();
  REQUIRE(catalogs.size() == 3);
  CHECK(catalogs[0].service == "llm");
  CHECK_FALSE(catalogs[0].configured);
  CHECK(catalogs[1].service == "tts");
  CHECK(catalogs[1].configured);
  CHECK(catalogs[1].reachable);
  CHECK(catalogs[1].configFile == "/srv/argus/config.tts.toml");
  REQUIRE(catalogs[1].profile.has_value());
  CHECK(catalogs[2].service == "vlm");
  CHECK_FALSE(catalogs[2].reachable);
  CHECK(gateway.catalogsOf({"vlm", "tts"}).size() == 1);

  const SettingsGatewayService plain({.owners = {{.name = "tts", .target = tts.target(), .credential = kSecret}},
                                      .timeouts = {.list = 1500ms, .update = 1500ms}});
  CHECK(plain.catalogs()[0].configFile == std::filesystem::absolute(configPath()).lexically_normal().string());
  std::filesystem::remove(configPath());
}

TEST_CASE("the catalogs of the owners a module that is off brings are left out, and every owner shows when nothing is hidden")
{
  const auto catalogs = [] {
    std::vector<OwnerCatalog> list;
    for (const auto* name : {"llm", "guard", "camera", "vlm", "tts"})
      list.push_back({.service = name, .reachable = true, .settings = {}});
    return list;
  };
  const auto names = [](const std::vector<OwnerCatalog>& list) {
    std::vector<std::string> out;
    out.reserve(list.size());
    for (const auto& catalog : list)
      out.push_back(catalog.service);
    return out;
  };
  const auto surveillanceOff = [](const std::string& owner) { return owner != "guard" && owner != "camera" && owner != "vlm"; };

  CHECK(names(owner_visibility::visible(catalogs(), surveillanceOff)) == std::vector<std::string>{"llm", "tts"});
  CHECK(names(owner_visibility::visible(catalogs(), {})).size() == 5);
  CHECK(names(owner_visibility::visible(catalogs(), [](const std::string&) { return true; })).size() == 5);
}
