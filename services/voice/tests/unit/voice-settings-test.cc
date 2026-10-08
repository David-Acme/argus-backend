#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/config-service.hxx>
#include <config/voice-config.hxx>
#include <feature/settings/voice-settings.hxx>
#include <feature/voice/voice-rpc-service.hxx>
#include <feature/voice/voice-session-service.hxx>
#include <grpc/grpc-client-base.hxx>
#include <settings/settings-rpc.hxx>
#include <shared/services/vad/vad-service.hxx>

#include <grpcpp/grpcpp.h>

#include <algorithm>
#include <array>
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

constexpr const char* kSyncSecret = "voice-settings-test-sync";
constexpr const char* kSettingsSecret = "voice-settings-test-settings";

class ScopedConfig
{
public:
  explicit ScopedConfig(const std::string& content)
      : path_((std::filesystem::temp_directory_path() / "voice-settings-test.toml").string())
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

std::string fallbackOf(const std::vector<SettingSpec>& catalog, std::string_view key)
{
  const auto spec = std::ranges::find(catalog, key, &SettingSpec::key);
  REQUIRE(spec != catalog.end());
  return spec->fallback;
}

std::string valueOf(const std::vector<SettingEntry>& entries, std::string_view key)
{
  const auto entry = std::ranges::find_if(entries, [key](const SettingEntry& candidate) {
    return candidate.spec.key == key;
  });
  REQUIRE(entry != entries.end());
  return entry->value;
}

struct CallerProbe
{
  std::string target;
  std::string secret;
};

grpc::Status connectWith(const CallerProbe& probe)
{
  const std::string& target = probe.target;
  const std::string& secret = probe.secret;
  auto stub = argus::voice::v1::VoiceService::NewStub(
      grpc::CreateChannel(target, grpc::InsecureChannelCredentials()));
  grpc::ClientContext context;
  context.AddMetadata("x-argus-user", "1");
  context.AddMetadata("x-argus-role", "owner");
  argus::client::addCallerCredential(context, secret);
  auto stream = stub->Connect(&context);
  stream->WritesDone();
  return stream->Finish();
}

grpc::Status listWith(const CallerProbe& probe)
{
  const std::string& target = probe.target;
  const std::string& secret = probe.secret;
  auto stub = wire::Settings::NewStub(grpc::CreateChannel(target, grpc::InsecureChannelCredentials()));
  grpc::ClientContext context;
  argus::client::addCallerCredential(context, secret);
  wire::SettingsCatalog catalog;
  return stub->List(&context, {}, &catalog);
}

}

TEST_CASE("the voice catalog builds through the registry and applies at the next session")
{
  const auto catalog = voiceSettingsCatalog();
  REQUIRE_NOTHROW(SettingsRegistry{voiceSettingsCatalog()});
  CHECK(catalog.size() == 14);
  for (const auto& spec : catalog)
    CHECK(spec.apply == SettingApply::NextSession);
  const auto basic = std::ranges::count(catalog, SettingLevel::Basic, &SettingSpec::level);
  CHECK(basic == 3);
}

TEST_CASE("the voice catalog names no plumbing")
{
  constexpr std::array kForbidden{"path", "file", "target", "url", "credential",
                                  "secret", "port", "address", "timeout"};
  for (const auto& spec : voiceSettingsCatalog())
    for (const std::string_view fragment : kForbidden)
      CHECK_MESSAGE(spec.key.find(fragment) == std::string::npos, spec.key);
}

TEST_CASE("every fallback is what the service runs with when the key is absent")
{
  const ScopedConfig config("");
  const auto catalog = voiceSettingsCatalog();
  const VadConfig vad = resolveVadConfig();
  const VoiceListeningConfig listening = resolveVoiceListeningConfig();
  CHECK(std::stof(fallbackOf(catalog, "vad.threshold")) == doctest::Approx(vad.threshold));
  CHECK(std::stof(fallbackOf(catalog, "vad.neg_threshold")) == doctest::Approx(vad.negThreshold));
  CHECK(std::stoi(fallbackOf(catalog, "vad.min_speech_frames")) == vad.minSpeechFrames);
  CHECK(std::stoi(fallbackOf(catalog, "vad.min_silence_frames")) == vad.minSilenceFrames);
  CHECK(std::stoi(fallbackOf(catalog, "vad.max_turn_frames")) == vad.maxTurnFrames);
  CHECK(std::stoi(fallbackOf(catalog, "vad.pre_roll_frames")) == vad.preRollFrames);
  CHECK(std::stoi(fallbackOf(catalog, "vad.min_turn_ms")) == vad.minTurnMs);
  CHECK(std::stof(fallbackOf(catalog, "vad.min_mean_prob")) == doctest::Approx(vad.minMeanProb));
  CHECK(std::stof(fallbackOf(catalog, "vad.barge_threshold")) == doctest::Approx(vad.bargeThreshold));
  CHECK(std::stoi(fallbackOf(catalog, "vad.barge_min_frames")) == vad.bargeMinFrames);
  CHECK(std::stoi(fallbackOf(catalog, "vad.barge_guard_ms")) == listening.bargeGuard.count());
  CHECK((fallbackOf(catalog, "vad.denoise") == "true") == listening.denoise);
  CHECK(std::stof(fallbackOf(catalog, "vad.denoise_gate_rms")) ==
        doctest::Approx(listening.denoiseGateRms));
}

TEST_CASE("a value changed through the registry is what the next session reads")
{
  const ScopedConfig config("[stt]\nlanguage = \"es\"\n\n[vad]\nthreshold = 0.45\n"
                            "min_silence_frames = 12\nbarge_threshold = 0.7\n"
                            "denoise = true\nbarge_guard_ms = 300\n");
  SettingsRegistry registry(voiceSettingsCatalog());
  std::vector<std::string> notified;
  registry.onChange([&notified](const std::vector<std::string>& keys) { notified = keys; });

  const auto result = registry.update({{.key = "stt.language", .value = "en"},
                                       {.key = "vad.min_silence_frames", .value = "20"},
                                       {.key = "vad.barge_threshold", .value = "0.85"},
                                       {.key = "vad.denoise", .value = "false"},
                                       {.key = "vad.barge_guard_ms", .value = "600"}});
  CHECK(result.rejected.empty());
  CHECK(notified.size() == 5);

  const VadConfig vad = resolveVadConfig();
  CHECK(vad.minSilenceFrames == 20);
  CHECK(vad.bargeThreshold == doctest::Approx(0.85F));
  CHECK(vad.threshold == doctest::Approx(0.45F));
  const VoiceListeningConfig listening = resolveVoiceListeningConfig();
  CHECK_FALSE(listening.denoise);
  CHECK(listening.bargeGuard.count() == 600);
  CHECK(voiceSystemLang() == VoiceLang::En);

  const auto entries = registry.list();
  CHECK(valueOf(entries, "vad.min_silence_frames") == "20");
  CHECK(valueOf(entries, "stt.language") == "en");
}

TEST_CASE("the registry refuses values the session could not use")
{
  const ScopedConfig config("[vad]\nmin_silence_frames = 12\n");
  SettingsRegistry registry(voiceSettingsCatalog());
  const auto result = registry.update({{.key = "vad.min_silence_frames", .value = "0"},
                                       {.key = "stt.language", .value = "fr"}});
  CHECK(result.applied.empty());
  REQUIRE(result.rejected.size() == 2);
  CHECK(result.rejected[0].reason == SettingRejectionReason::OutOfRange);
  CHECK(result.rejected[1].reason == SettingRejectionReason::NotAChoice);
  CHECK(resolveVadConfig().minSilenceFrames == 12);
}

TEST_CASE("the settings caller and the sync caller cannot stand in for each other")
{
  const ScopedConfig config(std::string("[grpc]\ncaller_sync = \"") + kSyncSecret +
                            "\"\ncaller_settings = \"" + kSettingsSecret + "\"\n");
  const auto settingsCallers = VoiceConfig::resolveSettingsCallers();
  REQUIRE(settingsCallers.size() == 1);
  CHECK(settingsCallers[0].service == kSettingsCaller);

  SettingsRegistry registry(voiceSettingsCatalog());
  VoiceSessionService sessions;
  VoiceRpcService voice({.sessions = &sessions,
                         .syncCallerSecret = VoiceConfig::resolveSyncCallerSecret(),
                         .notificationCallerSecret = VoiceConfig::resolveNotificationCallerSecret(),
                         .rooms = nullptr,
                         .dispatchCleanup = {}});
  SettingsRpcService settings(
      SettingsRpcInput{.service = "voice", .registry = &registry, .credentials = settingsCallers});
  int port = 0;
  grpc::ServerBuilder builder;
  builder.AddListeningPort("127.0.0.1:0", grpc::InsecureServerCredentials(), &port);
  builder.RegisterService(&voice);
  builder.RegisterService(&settings);
  const std::unique_ptr<grpc::Server> server(builder.BuildAndStart());
  REQUIRE(server);
  const std::string target = "127.0.0.1:" + std::to_string(port);

  CHECK(listWith({.target = target, .secret = kSettingsSecret}).ok());
  CHECK(listWith({.target = target, .secret = kSyncSecret}).error_code() == grpc::StatusCode::UNAUTHENTICATED);
  CHECK(connectWith({.target = target, .secret = kSettingsSecret}).error_code() == grpc::StatusCode::UNAUTHENTICATED);
  CHECK(connectWith({.target = target, .secret = kSyncSecret}).ok());

  server->Shutdown();
}

TEST_CASE("a settings secret equal to the sync secret publishes no settings caller")
{
  const ScopedConfig config(std::string("[grpc]\ncaller_sync = \"") + kSyncSecret +
                            "\"\ncaller_settings = \"" + kSyncSecret + "\"\n");
  CHECK(VoiceConfig::resolveSettingsCallers().empty());
}

TEST_CASE("an empty settings secret publishes no settings caller")
{
  const ScopedConfig config(std::string("[grpc]\ncaller_sync = \"") + kSyncSecret + "\"\n");
  CHECK(VoiceConfig::resolveSettingsCallers().empty());
}
