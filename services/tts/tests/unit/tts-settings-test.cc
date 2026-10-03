#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/config-service.hxx>
#include <config/settings-registry.hxx>
#include <feature/settings/tts-settings.hxx>
#include <feature/synthesis/services/tts-service.hxx>

#include <algorithm>
#include <array>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

namespace
{
class ScopedConfig
{
public:
  explicit ScopedConfig(const std::string& content)
      : path_((std::filesystem::temp_directory_path() / "tts-settings-test.toml").string())
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

std::string decimalFallback(float value)
{
  std::string text = std::to_string(value);
  while (text.ends_with('0'))
    text.pop_back();
  if (text.ends_with('.'))
    text.pop_back();
  return text;
}
}

TEST_CASE("the tts catalog builds through the registry")
{
  REQUIRE_NOTHROW(SettingsRegistry{ttsSettingsCatalog()});
  const auto catalog = ttsSettingsCatalog();
  CHECK(specOf(catalog, "tts.engine_es").level == SettingLevel::Basic);
  CHECK(specOf(catalog, "tts.engine_en").level == SettingLevel::Basic);
  CHECK(specOf(catalog, "tts.pocket_variant_es").level == SettingLevel::Basic);
  for (const auto* key : {"tts.engine_es", "tts.engine_en", "tts.pocket_variant_es", "tts.pocket_voice_es",
                          "tts.pocket_voice_en", "tts.pocket_temperature", "tts.pocket_lsd_steps",
                          "tts.pocket_reference_es", "tts.pocket_reference_en", "tts.normalize_text"})
    CHECK_MESSAGE(specOf(catalog, key).apply == SettingApply::Live, key);
  CHECK(specOf(catalog, "tts.pocket_voice_es").level == SettingLevel::Advanced);
  CHECK(specOf(catalog, "tts.normalize_text").level == SettingLevel::Advanced);
}

TEST_CASE("the tts catalog names no plumbing")
{
  constexpr std::array kForbidden{"path", "file", "dir", "target", "url", "credential", "secret", "port", "address"};
  for (const auto& spec : ttsSettingsCatalog())
    for (const std::string_view fragment : kForbidden)
      CHECK_MESSAGE(spec.key.find(fragment) == std::string::npos, spec.key);
}

TEST_CASE("every engine fallback is what the service runs with when the key is absent")
{
  const ScopedConfig config("");
  const auto catalog = ttsSettingsCatalog();
  CHECK(specOf(catalog, "tts.engine_es").fallback == TtsService::engineName(TtsService::configuredEngine(TtsLang::ES)));
  CHECK(specOf(catalog, "tts.engine_en").fallback == TtsService::engineName(TtsService::configuredEngine(TtsLang::EN)));
  CHECK(specOf(catalog, "tts.pocket_variant_es").fallback == TtsService::configuredPocketVariant(TtsLang::ES));
  CHECK(specOf(catalog, "tts.pocket_voice_es").fallback == TtsService::configuredPocketVoice(TtsLang::ES));
  CHECK(specOf(catalog, "tts.pocket_voice_en").fallback == TtsService::configuredPocketVoice(TtsLang::EN));
  CHECK(specOf(catalog, "tts.pocket_temperature").fallback == decimalFallback(TtsService::configuredPocketTemperature()));
  CHECK(specOf(catalog, "tts.pocket_lsd_steps").fallback == std::to_string(TtsService::configuredPocketLsdSteps()));
  CHECK(specOf(catalog, "tts.normalize_text").fallback == (TtsService::normalizationEnabled() ? "true" : "false"));
  CHECK(specOf(catalog, "tts.pocket_reference_es").fallback.empty());
  CHECK(TtsService::configuredEngine(TtsLang::FR) == SpeechEngineKind::Supertonic);
}

TEST_CASE("every voice choice is a voice the service accepts as itself")
{
  const ScopedConfig config("");
  const auto catalog = ttsSettingsCatalog();
  CHECK(specOf(catalog, "tts.pocket_voice_es").choices == TtsService::pocketVoices(TtsLang::ES));
  CHECK(specOf(catalog, "tts.pocket_voice_en").choices == TtsService::pocketVoices(TtsLang::EN));
  SettingsRegistry registry(ttsSettingsCatalog());
  for (const auto& voice : TtsService::pocketVoices(TtsLang::ES)) {
    REQUIRE(registry.update({{.key = "tts.pocket_voice_es", .value = voice}}).rejected.empty());
    CHECK(TtsService::configuredPocketVoice(TtsLang::ES) == voice);
  }
  CHECK(registry.update({{.key = "tts.pocket_voice_es", .value = "george"}}).rejected.size() == 1);
}

TEST_CASE("the engine for each language follows the registry live")
{
  const ScopedConfig config("[tts]\nengine_es = \"pocket\"\nengine_en = \"pocket\"\n");
  SettingsRegistry registry(ttsSettingsCatalog());
  CHECK(TtsService::configuredEngine(TtsLang::ES) == SpeechEngineKind::Pocket);
  REQUIRE(registry.update({{.key = "tts.engine_es", .value = "supertonic"}}).rejected.empty());
  CHECK(TtsService::configuredEngine(TtsLang::ES) == SpeechEngineKind::Supertonic);
  CHECK(TtsService::configuredEngine(TtsLang::EN) == SpeechEngineKind::Pocket);
  REQUIRE(registry.update({{.key = "tts.engine_en", .value = "supertonic"}}).rejected.empty());
  CHECK(TtsService::configuredEngine(TtsLang::EN) == SpeechEngineKind::Supertonic);
  REQUIRE(registry.update({{.key = "tts.engine_es", .value = "pocket"}}).rejected.empty());
  CHECK(TtsService::configuredEngine(TtsLang::ES) == SpeechEngineKind::Pocket);
  CHECK(registry.update({{.key = "tts.engine_es", .value = "kokoro"}}).rejected.size() == 1);
  REQUIRE(registry.update({{.key = "tts.pocket_variant_es", .value = "quality"}}).rejected.empty());
  CHECK(TtsService::configuredPocketVariant(TtsLang::ES) == "quality");
}

TEST_CASE("normalization is a live toggle applied before every engine")
{
  const ScopedConfig config("");
  SettingsRegistry registry(ttsSettingsCatalog());
  const TtsRequest request{.text = "Son las 14:30", .lang = TtsLang::ES, .voiceId = "M3", .quality = TtsQuality::Auto,
                           .speed = 1.0F};
  CHECK(TtsService::speechText(request) == "Son las dos y media de la tarde");
  REQUIRE(registry.update({{.key = "tts.normalize_text", .value = "false"}}).rejected.empty());
  CHECK(TtsService::speechText(request) == "Son las 14:30");
}
