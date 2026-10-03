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
           .fallback = "1"},
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
  const auto build = [](SettingsClientConfig config) {
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
