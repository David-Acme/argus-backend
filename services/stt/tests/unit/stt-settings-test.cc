#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <app/rpc/stt-rpc-server.hxx>
#include <config/config-service.hxx>
#include <config/stt-config.hxx>
#include <errors/response-exception.hxx>
#include <feature/settings/stt-settings.hxx>
#include <feature/stt/services/stt-service.hxx>
#include <grpc/grpc-client-base.hxx>
#include <settings/settings-rpc.hxx>
#include <stt/stt-client.hxx>
#include <stt/stt-remote.hxx>

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

constexpr const char* kVoiceSecret = "stt-settings-test-voice";
constexpr const char* kSettingsSecret = "stt-settings-test-settings";

class ScopedConfig
{
public:
  explicit ScopedConfig(const std::string& content)
      : path_((std::filesystem::temp_directory_path() / "stt-settings-test.toml").string())
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

std::string callersConfig()
{
  return std::string("[rpc]\naddress = \"127.0.0.1:0\"\n\n[rpc.callers]\nvoice = \"") + kVoiceSecret +
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

int capabilitiesStatusWith(int port, const std::string& secret)
{
  argus::stt::Client client({.target = "127.0.0.1:" + std::to_string(port),
                             .credential = secret,
                             .timeout = std::chrono::seconds(5)});
  try {
    static_cast<void>(client.capabilities());
    return 200;
  }
  catch (const ResponseException& error) {
    return error.statusCode();
  }
}

SttRpcInput fakeInput(const SttRpcConfig& config, SettingsRpcService& settings)
{
  return {.address = config.address,
          .credentials = config.credentials,
          .capabilities =
              [] {
                return argus::stt::Capabilities{.sampleRate = kWireSampleRate,
                                                .loaded = true,
                                                .language = "es",
                                                .defaultLanguage = "es",
                                                .languages = {"es", "en", "auto"}};
              },
          .acceptsLanguage = [](const std::string&) { return true; },
          .transcribe = [](const TranscribeRequest&) { return std::string("heard"); },
          .slots = 1,
          .services = {&settings}};
}
}

TEST_CASE("the stt catalog builds through the registry")
{
  const auto catalog = sttSettingsCatalog();
  REQUIRE_NOTHROW(SettingsRegistry{sttSettingsCatalog()});
  CHECK(catalog.size() == 2);
  CHECK(specOf(catalog, "stt.language").apply == SettingApply::Live);
  CHECK(specOf(catalog, "stt.engine").apply == SettingApply::Restart);
  for (const auto& spec : catalog)
    CHECK(spec.level == SettingLevel::Advanced);
}

TEST_CASE("the stt catalog names no plumbing")
{
  constexpr std::array kForbidden{"path", "file", "dir", "target", "url",
                                  "credential", "secret", "port", "address"};
  for (const auto& spec : sttSettingsCatalog())
    for (const std::string_view fragment : kForbidden)
      CHECK_MESSAGE(spec.key.find(fragment) == std::string::npos, spec.key);
}

TEST_CASE("every fallback is what the service runs with when the key is absent")
{
  const ScopedConfig config("");
  const auto catalog = sttSettingsCatalog();
  CHECK(specOf(catalog, "stt.language").fallback == SttService::configLanguage());
  CHECK(specOf(catalog, "stt.engine").fallback == SttService::engineName(SttService::configEngine()));
}

TEST_CASE("every choice is a value the engine accepts as itself")
{
  const ScopedConfig config("");
  const auto catalog = sttSettingsCatalog();
  CHECK(specOf(catalog, "stt.language").choices == SttService::supportedLanguages());
  SettingsRegistry registry(sttSettingsCatalog());
  for (const auto& choice : specOf(catalog, "stt.engine").choices) {
    REQUIRE(registry.update({{.key = "stt.engine", .value = choice}}).rejected.empty());
    CHECK(SttService::engineName(SttService::configEngine()) == choice);
  }
}

TEST_CASE("a changed default language is what the next request without one resolves")
{
  const ScopedConfig config("[stt]\nlanguage = \"es\"\n");
  SettingsRegistry registry(sttSettingsCatalog());
  CHECK(SttService::configLanguage() == "es");
  const auto result = registry.update({{.key = "stt.language", .value = "en"}});
  CHECK(result.rejected.empty());
  CHECK(SttService::configLanguage() == "en");
  CHECK(registry.update({{.key = "stt.language", .value = "fr"}}).rejected.size() == 1);
  CHECK(SttService::configLanguage() == "en");
}

TEST_CASE("the settings caller is split from the transcription callers")
{
  const ScopedConfig config(callersConfig());
  const SttRpcConfig rpc = SttConfig::resolveRpc();
  REQUIRE(rpc.credentials.size() == 1);
  CHECK(rpc.credentials[0].first == "voice");
  REQUIRE(rpc.settingsCredentials.size() == 1);
  CHECK(rpc.settingsCredentials[0].service == kSettingsCaller);
}

TEST_CASE("the settings caller and a transcription caller cannot stand in for each other")
{
  const ScopedConfig config(callersConfig());
  const SttRpcConfig rpc = SttConfig::resolveRpc();
  SettingsRegistry registry(sttSettingsCatalog());
  SettingsRpcService settings(
      SettingsRpcInput{.service = "stt", .registry = &registry, .credentials = rpc.settingsCredentials});
  SttRpcServer server(fakeInput(rpc, settings));
  const std::string target = "127.0.0.1:" + std::to_string(server.port());

  CHECK(listWith(target, kSettingsSecret).ok());
  CHECK(listWith(target, kVoiceSecret).error_code() == grpc::StatusCode::UNAUTHENTICATED);
  CHECK(capabilitiesStatusWith(server.port(), kSettingsSecret) == 401);
  CHECK(capabilitiesStatusWith(server.port(), kVoiceSecret) == 200);

  server.shutdown();
}
