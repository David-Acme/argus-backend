#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/camera-config.hxx>
#include <config/config-service.hxx>
#include <config/operator-config.hxx>
#include <feature/actions/camera-action-rpc-service.hxx>
#include <feature/media/camera-media-service.hxx>
#include <feature/settings/camera-settings.hxx>
#include <feature/sync/camera-sync-rpc-service.hxx>
#include <grpc/grpc-client-base.hxx>
#include <settings/settings-rpc.hxx>
#include <shared/services/stream/stream-hub.hxx>

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace
{
namespace wire = argus::settings::v1;

constexpr const char* kGuardSecret = "camera-settings-test-guard";
constexpr const char* kSyncSecret = "camera-settings-test-sync";
constexpr const char* kLlmSecret = "camera-settings-test-llm";
constexpr const char* kSettingsSecret = "camera-settings-test-settings";

class ScopedConfig
{
public:
  explicit ScopedConfig(const std::string& content)
      : path_((std::filesystem::temp_directory_path() / "camera-settings-test.toml").string())
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

std::string callersConfig(const std::string& settingsSecret)
{
  return std::string("[grpc]\ncaller_guard = \"") + kGuardSecret + "\"\ncaller_sync = \"" +
         kSyncSecret + "\"\ncaller_llm = \"" + kLlmSecret + "\"\ncaller_settings = \"" +
         settingsSecret + "\"\n";
}

const SettingSpec& specOf(const std::vector<SettingSpec>& catalog, std::string_view key)
{
  const auto spec = std::ranges::find(catalog, key, &SettingSpec::key);
  REQUIRE(spec != catalog.end());
  return *spec;
}

std::string valueOf(const std::vector<SettingEntry>& entries, std::string_view key)
{
  const auto entry = std::ranges::find_if(entries, [key](const SettingEntry& candidate) {
    return candidate.spec.key == key;
  });
  REQUIRE(entry != entries.end());
  return entry->value;
}

grpc::StatusCode listWith(const std::string& target, const std::string& secret)
{
  auto stub = wire::Settings::NewStub(grpc::CreateChannel(target, grpc::InsecureChannelCredentials()));
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
  argus::client::addCallerCredential(context, secret);
  wire::SettingsCatalog catalog;
  return stub->List(&context, {}, &catalog).error_code();
}

grpc::StatusCode pullWith(const std::string& target, const std::string& secret)
{
  auto stub = argus::camera::v1::SyncService::NewStub(
      grpc::CreateChannel(target, grpc::InsecureChannelCredentials()));
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
  argus::client::addCallerIdentity(context, {.userId = 1, .role = "owner", .device = "test"});
  argus::client::addCallerCredential(context, secret);
  argus::camera::v1::PullTableResponse response;
  return stub->PullTable(&context, {}, &response).error_code();
}

grpc::StatusCode personCropWith(const std::string& target, const std::string& secret)
{
  auto stub = argus::camera::v1::CameraActionService::NewStub(
      grpc::CreateChannel(target, grpc::InsecureChannelCredentials()));
  grpc::ClientContext context;
  context.set_deadline(std::chrono::system_clock::now() + std::chrono::seconds(5));
  argus::client::addCallerCredential(context, secret);
  argus::camera::v1::PersonCropResponse response;
  return stub->GetPersonCrop(&context, {}, &response).error_code();
}

}

TEST_CASE("the camera catalog builds through the registry")
{
  const auto catalog = cameraSettingsCatalog();
  REQUIRE_NOTHROW(SettingsRegistry{cameraSettingsCatalog()});
  CHECK(catalog.size() == 17);
  CHECK(std::ranges::count(catalog, SettingLevel::Basic, &SettingSpec::level) == 6);

  std::vector<std::string> live;
  for (const auto& spec : catalog)
    if (spec.apply == SettingApply::Live)
      live.push_back(spec.key);
  CHECK(live == std::vector<std::string>{"actions.enabled", "streaming.max_viewers_per_camera",
                                         "streaming.max_total_viewers"});
  CHECK(specOf(catalog, "streaming.hub_window_bytes").apply == SettingApply::NextSession);
}

TEST_CASE("the camera catalog names no plumbing")
{
  constexpr std::array kForbidden{"path", "file", "target", "url", "credential", "secret",
                                  "port", "address", "timeout", "model", "db", "schema",
                                  "bin", "dir", "caller", "go2rtc", "vulkan"};
  for (const auto& spec : cameraSettingsCatalog())
    for (const std::string_view fragment : kForbidden)
      CHECK_MESSAGE(spec.key.find(fragment) == std::string::npos, spec.key);
}

TEST_CASE("every fallback is what the camera runs with when the key is absent")
{
  const ScopedConfig config("");
  const auto catalog = cameraSettingsCatalog();
  const auto fallback = [&catalog](std::string_view key) { return specOf(catalog, key).fallback; };

  const ObjectsConfig objects = operator_config::resolveObjects();
  CHECK((fallback("objects.enabled") == "true") == objects.enabled);
  CHECK(std::stof(fallback("objects.conf")) == doctest::Approx(objects.confidence));

  const OperatorConfig operatorConfig = operator_config::resolveOperator();
  CHECK(std::stoi(fallback("operator.night_start")) == operatorConfig.nightStartHour);
  CHECK(std::stoi(fallback("operator.night_end")) == operatorConfig.nightEndHour);
  CHECK(std::stoll(fallback("operator.cooldown_ms")) == operatorConfig.cooldownMs);
  CHECK(std::stoll(fallback("operator.person_recheck_ms")) == operatorConfig.personRecheckMs);

  CHECK((fallback("actions.enabled") == "true") == ConfigService::getBool("actions.enabled"));

  StreamHub hub;
  hub.refreshViewerLimits();
  const StreamHub::ViewerLimits limits = hub.viewerLimits();
  CHECK(std::stoi(fallback("streaming.max_viewers_per_camera")) == limits.perCamera);
  CHECK(std::stoi(fallback("streaming.max_total_viewers")) == limits.total);
  CHECK(std::stoll(fallback("streaming.hub_window_bytes")) ==
        CameraMediaService::streamWindowBytes());

  const CameraHealthConfig health = CameraConfig::resolveHealth();
  CHECK((fallback("health.enabled") == "true") == health.enabled);
  CHECK(std::stoll(fallback("health.interval_ms")) == health.intervalMs);
  CHECK(std::stod(fallback("health.dark_threshold")) == doctest::Approx(health.thresholds.dark));
  CHECK(std::stod(fallback("health.bright_threshold")) ==
        doctest::Approx(health.thresholds.bright));
  CHECK(std::stod(fallback("health.blur_threshold")) == doctest::Approx(health.thresholds.blur));
  CHECK(std::stod(fallback("health.scene_diff")) == doctest::Approx(health.thresholds.sceneDiff));
  CHECK(std::stoll(fallback("health.rebaseline_after_s")) * 1000 == health.rebaselineAfterMs);
}

TEST_CASE("a viewer limit changed through the registry reaches the hub without a restart")
{
  const ScopedConfig config("[streaming]\nmax_viewers_per_camera = 4\nmax_total_viewers = 8\n"
                            "\n[actions]\nenabled = false\n");
  StreamHub hub;
  hub.refreshViewerLimits();
  SettingsRegistry registry(cameraSettingsCatalog());
  std::vector<std::string> notified;
  registry.onChange([&hub, &notified](const std::vector<std::string>& keys) {
    notified = keys;
    hub.refreshViewerLimits();
  });

  const auto result = registry.update({{.key = "streaming.max_viewers_per_camera", .value = "2"},
                                       {.key = "streaming.max_total_viewers", .value = "12"},
                                       {.key = "actions.enabled", .value = "true"}});
  CHECK(result.rejected.empty());
  CHECK(notified.size() == 3);
  const StreamHub::ViewerLimits limits = hub.viewerLimits();
  CHECK(limits.perCamera == 2);
  CHECK(limits.total == 12);
  CHECK(ConfigService::getBool("actions.enabled"));
  CHECK(valueOf(registry.list(), "streaming.max_total_viewers") == "12");
}

TEST_CASE("a changed detection or night window is what the next boot resolves")
{
  const ScopedConfig config("[objects]\nconf = 0.45\n\n[operator]\nnight_start = 22\n"
                            "night_end = 6\n");
  SettingsRegistry registry(cameraSettingsCatalog());
  const auto result = registry.update({{.key = "objects.conf", .value = "0.6"},
                                       {.key = "operator.night_start", .value = "0"},
                                       {.key = "operator.night_end", .value = "5"}});
  CHECK(result.rejected.empty());
  CHECK(operator_config::resolveObjects().confidence == doctest::Approx(0.6F));
  const OperatorConfig operatorConfig = operator_config::resolveOperator();
  CHECK(operatorConfig.nightStartHour == 0);
  CHECK(operatorConfig.nightEndHour == 5);
}

TEST_CASE("the registry refuses values the camera could not use")
{
  const ScopedConfig config("[streaming]\nmax_total_viewers = 8\n");
  SettingsRegistry registry(cameraSettingsCatalog());
  const auto result = registry.update({{.key = "streaming.max_total_viewers", .value = "0"},
                                       {.key = "operator.night_start", .value = "24"},
                                       {.key = "streaming.go2rtc_bin", .value = "/bin/sh"}});
  CHECK(result.applied.empty());
  REQUIRE(result.rejected.size() == 3);
  CHECK(result.rejected[0].reason == SettingRejectionReason::OutOfRange);
  CHECK(result.rejected[1].reason == SettingRejectionReason::OutOfRange);
  CHECK(result.rejected[2].reason == SettingRejectionReason::Unknown);
  CHECK(ConfigService::getInt("streaming.max_total_viewers") == 8);
}

TEST_CASE("the settings caller and the camera's other callers cannot stand in for each other")
{
  const ScopedConfig config(callersConfig(kSettingsSecret));
  const auto settingsCallers = CameraConfig::resolveSettingsCallers();
  REQUIRE(settingsCallers.size() == 1);
  CHECK(settingsCallers[0].service == kSettingsCaller);

  SettingsRegistry registry(cameraSettingsCatalog());
  CameraSyncRpcService sync(
      {argus::client::CallerCredential{.service = "argus-sync",
                                       .secret = CameraConfig::resolveSyncCallerSecret()},
       argus::client::CallerCredential{.service = "argus-llm",
                                       .secret = CameraConfig::resolveLlmCallerSecret()}});
  CameraActionRpcService actions(
      {.callers = {argus::client::CallerCredential{
           .service = "argus-guard", .secret = CameraConfig::resolveGuardCallerSecret()}},
       .transcriber = makeHttpSttTranscriber()});
  SettingsRpcService settings(
      SettingsRpcInput{.service = "camera", .registry = &registry, .credentials = settingsCallers});
  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&sync);
  builder.RegisterService(&actions);
  builder.RegisterService(&settings);
  const std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  REQUIRE(server);
  const std::string target = "127.0.0.1:" + std::to_string(port);

  CHECK(listWith(target, kSettingsSecret) == grpc::StatusCode::OK);
  CHECK(listWith(target, kGuardSecret) == grpc::StatusCode::UNAUTHENTICATED);
  CHECK(listWith(target, kSyncSecret) == grpc::StatusCode::UNAUTHENTICATED);
  CHECK(listWith(target, kLlmSecret) == grpc::StatusCode::UNAUTHENTICATED);

  CHECK(pullWith(target, kSettingsSecret) == grpc::StatusCode::UNAUTHENTICATED);
  CHECK(pullWith(target, kSyncSecret) == grpc::StatusCode::INVALID_ARGUMENT);
  CHECK(pullWith(target, kLlmSecret) == grpc::StatusCode::INVALID_ARGUMENT);

  CHECK(personCropWith(target, kSettingsSecret) == grpc::StatusCode::UNAUTHENTICATED);
  CHECK(personCropWith(target, kGuardSecret) == grpc::StatusCode::INVALID_ARGUMENT);

  server->Shutdown(std::chrono::system_clock::now());
}

TEST_CASE("a settings secret equal to another caller's publishes no settings caller")
{
  for (const char* reused : {kGuardSecret, kSyncSecret, kLlmSecret}) {
    const ScopedConfig config(callersConfig(reused));
    CHECK(CameraConfig::resolveSettingsCallers().empty());
  }
}

TEST_CASE("an empty settings secret publishes no settings caller")
{
  const ScopedConfig config(callersConfig(""));
  CHECK(CameraConfig::resolveSettingsCallers().empty());
}
