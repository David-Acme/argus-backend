#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <app/rpc/vlm-rpc-server.hxx>
#include <config/config-service.hxx>
#include <config/vlm-config.hxx>
#include <errors/response-exception.hxx>
#include <feature/settings/vlm-settings.hxx>
#include <feature/vlm/services/vision-service.hxx>
#include <grpc/grpc-client-base.hxx>
#include <settings/settings-rpc.hxx>
#include <vlm/vlm-client.hxx>

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace wire = argus::settings::v1;

constexpr const char* kGuardSecret = "vlm-settings-test-guard";
constexpr const char* kSettingsSecret = "vlm-settings-test-settings";

class ScopedConfig
{
public:
  explicit ScopedConfig(const std::string& content)
      : path_((std::filesystem::temp_directory_path() / "vlm-settings-test.toml").string())
  {
    std::ofstream(path_) << content;
    ConfigService::load(path_);
  }

  ~ScopedConfig()
  {
    std::ofstream(path_).flush();
    ConfigService::load(path_);
    std::remove(path_.c_str());
  }

  ScopedConfig(const ScopedConfig&) = delete;
  ScopedConfig& operator=(const ScopedConfig&) = delete;

private:
  std::string path_;
};

const SettingSpec& specOf(const std::vector<SettingSpec>& catalog, std::string_view key)
{
  const auto spec = std::ranges::find(catalog, key, &SettingSpec::key);
  REQUIRE(spec != catalog.end());
  return *spec;
}

int fallbackInt(const std::vector<SettingSpec>& catalog, std::string_view key)
{
  return std::stoi(specOf(catalog, key).fallback);
}

std::string callersConfig()
{
  return std::string("[rpc]\naddress = \"127.0.0.1:0\"\n\n[rpc.callers]\nguard = \"") + kGuardSecret +
         "\"\nsettings = \"" + kSettingsSecret + "\"\n";
}

grpc::Status listWith(const std::string& target, const std::string& secret)
{
  auto stub = wire::Settings::NewStub(grpc::CreateChannel(target, grpc::InsecureChannelCredentials()));
  grpc::ClientContext context;
  argus::client::addCallerCredential(context, secret);
  wire::SettingsCatalog catalog;
  return stub->List(&context, {}, &catalog);
}

argus::vlm::Client clientWith(int port, const std::string& secret)
{
  return argus::vlm::Client({.target = "127.0.0.1:" + std::to_string(port),
                             .credential = secret,
                             .timeout = std::chrono::seconds(5)});
}

int capabilitiesStatusWith(int port, const std::string& secret)
{
  const auto client = clientWith(port, secret);
  try {
    static_cast<void>(client.capabilities());
    return 200;
  }
  catch (const ResponseException& error) {
    return error.statusCode();
  }
}

VlmRpcInput serverInput(const VlmRpcConfig& config, VisionService& vision, SettingsRpcService& settings)
{
  return {.address = config.address,
          .credentials = config.credentials,
          .capabilities =
              [&vision] {
                return argus::vlm::Capabilities{.loaded = true,
                                                .maxInputPx = vision.maxInputPx(),
                                                .defaultMaxTokens = vision.defaultMaxTokens()};
              },
          .describe = [](const VisionDescribeMatInput&) { return std::string("a quiet yard"); },
          .slots = 1,
          .services = {&settings}};
}
}

TEST_CASE("the vlm catalog builds through the registry")
{
  const auto catalog = vlmSettingsCatalog();
  REQUIRE_NOTHROW(SettingsRegistry{vlmSettingsCatalog()});
  CHECK(catalog.size() == 8);
  CHECK(std::ranges::count(catalog, SettingLevel::Basic, &SettingSpec::level) == 2);
  CHECK(specOf(catalog, "vision.max_input_px").level == SettingLevel::Basic);
  CHECK(specOf(catalog, "vision.max_tokens").level == SettingLevel::Basic);
  for (const std::string_view key : {"vision.max_input_px", "vision.max_tokens", "vision.prompt",
                                     "vision.caption_cache_slots"})
    CHECK(specOf(catalog, key).apply == SettingApply::Live);
  for (const std::string_view key : {"vision.image_max_tokens", "vision.context_size", "vision.threads",
                                     "vision.gpu_layers"})
    CHECK(specOf(catalog, key).apply == SettingApply::Restart);
}

TEST_CASE("the vlm catalog names no plumbing")
{
  constexpr std::array kForbidden{"path", "file", "dir", "target", "url",
                                  "credential", "secret", "port", "address", "model"};
  for (const auto& spec : vlmSettingsCatalog())
    for (const std::string_view fragment : kForbidden)
      CHECK_MESSAGE(spec.key.find(fragment) == std::string::npos, spec.key);
}

TEST_CASE("every fallback is what the service runs with when the key is absent")
{
  const ScopedConfig config("");
  const auto catalog = vlmSettingsCatalog();
  const VisionDefaults defaults = resolveVisionDefaults();
  const VisionEngineSettings engine = resolveVisionEngineSettings();
  CHECK(fallbackInt(catalog, "vision.max_input_px") == defaults.maxInputPx);
  CHECK(fallbackInt(catalog, "vision.max_tokens") == defaults.maxTokens);
  CHECK(specOf(catalog, "vision.prompt").fallback == defaults.prompt);
  CHECK(fallbackInt(catalog, "vision.caption_cache_slots") == defaults.cacheSlots);
  CHECK(fallbackInt(catalog, "vision.image_max_tokens") == engine.imageMaxTokens);
  CHECK(fallbackInt(catalog, "vision.context_size") == engine.contextSize);
  CHECK(fallbackInt(catalog, "vision.threads") == engine.threads);
  CHECK(fallbackInt(catalog, "vision.gpu_layers") == engine.gpuLayers);
}

TEST_CASE("a value changed through the registry is what the next description runs with")
{
  const ScopedConfig config("[vision]\nmax_input_px = 384\nmax_tokens = 64\nprompt = \"Describe it.\"\n");
  VisionService vision;
  vision.refreshDefaults();
  SettingsRegistry registry(vlmSettingsCatalog());
  registry.onChange([&vision](const std::vector<std::string>&) { vision.refreshDefaults(); });
  CHECK(vision.maxInputPx() == 384);
  CHECK(vision.defaultMaxTokens() == 64);

  const auto result = registry.update({{.key = "vision.max_input_px", .value = "512"},
                                       {.key = "vision.max_tokens", .value = "128"},
                                       {.key = "vision.prompt", .value = "Who is at the door?"}});
  CHECK(result.rejected.empty());
  CHECK(vision.maxInputPx() == 512);
  CHECK(vision.defaultMaxTokens() == 128);
  CHECK(resolveVisionDefaults().prompt == "Who is at the door?");

  const auto refused = registry.update({{.key = "vision.max_input_px", .value = "64"}});
  REQUIRE(refused.rejected.size() == 1);
  CHECK(refused.rejected[0].reason == SettingRejectionReason::OutOfRange);
  CHECK(vision.maxInputPx() == 512);
}

TEST_CASE("the settings caller is split from the vision callers")
{
  const ScopedConfig config(callersConfig());
  const VlmRpcConfig rpc = VlmConfig::resolveRpc();
  REQUIRE(rpc.credentials.size() == 1);
  CHECK(rpc.credentials[0].first == "guard");
  REQUIRE(rpc.settingsCredentials.size() == 1);
  CHECK(rpc.settingsCredentials[0].service == kSettingsCaller);
}

TEST_CASE("the settings caller and a vision caller cannot stand in for each other")
{
  const ScopedConfig config(callersConfig() + "\n[vision]\nmax_input_px = 384\n");
  const VlmRpcConfig rpc = VlmConfig::resolveRpc();
  VisionService vision;
  vision.refreshDefaults();
  SettingsRegistry registry(vlmSettingsCatalog());
  registry.onChange([&vision](const std::vector<std::string>&) { vision.refreshDefaults(); });
  SettingsRpcService settings(
      SettingsRpcInput{.service = "vlm", .registry = &registry, .credentials = rpc.settingsCredentials});
  VlmRpcServer server(serverInput(rpc, vision, settings));
  const std::string target = "127.0.0.1:" + std::to_string(server.port());

  CHECK(listWith(target, kSettingsSecret).ok());
  CHECK(listWith(target, kGuardSecret).error_code() == grpc::StatusCode::UNAUTHENTICATED);
  CHECK(capabilitiesStatusWith(server.port(), kSettingsSecret) == 401);
  CHECK(capabilitiesStatusWith(server.port(), kGuardSecret) == 200);

  CHECK(clientWith(server.port(), kGuardSecret).capabilities().maxInputPx == 384);
  REQUIRE(registry.update({{.key = "vision.max_input_px", .value = "640"}}).rejected.empty());
  CHECK(clientWith(server.port(), kGuardSecret).capabilities().maxInputPx == 640);

  server.shutdown();
}
