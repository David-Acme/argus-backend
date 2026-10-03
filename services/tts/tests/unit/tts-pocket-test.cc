#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <config/config-service.hxx>
#include <feature/synthesis/infra/pocket/pcm-rate-converter.hxx>
#include <feature/synthesis/infra/pocket/pocket-engine.hxx>
#include <feature/synthesis/infra/pocket/reference-audio.hxx>
#include <feature/synthesis/infra/pocket/unigram-tokenizer.hxx>
#include <feature/synthesis/services/tts-service.hxx>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <string>
#include <string_view>
#include <vector>

#ifndef ARGUS_TEST_TTS_MODELS_DIR
#define ARGUS_TEST_TTS_MODELS_DIR "models/tts"
#endif

namespace
{
constexpr std::string_view kModelsRoot{ARGUS_TEST_TTS_MODELS_DIR};

std::filesystem::path modelsRoot()
{
  return {kModelsRoot};
}

std::filesystem::path pocketRoot()
{
  return modelsRoot() / "pocket";
}

bool provisioned(const std::string& variant)
{
  return std::filesystem::is_regular_file(pocketRoot() / variant / "bundle.json") &&
         std::filesystem::is_regular_file(pocketRoot() / variant / "voices" / (variant == "en" ? "alba.safetensors" : "lola.safetensors"));
}

class ScopedConfig
{
public:
  explicit ScopedConfig(const std::string& content)
      : path_((std::filesystem::temp_directory_path() / "tts-pocket-test.toml").string())
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

std::size_t zeroCrossings(const std::vector<float>& samples)
{
  std::size_t crossings = 0;
  for (std::size_t index = 1; index < samples.size(); ++index)
    crossings += (samples[index - 1] < 0.0F) != (samples[index] < 0.0F) ? 1 : 0;
  return crossings;
}

struct Synthesis
{
  std::vector<float> samples;
  std::size_t calls{0};
};

struct StreamRun
{
  PocketEngine& engine;
  const PocketVoice& voice;
  std::string text;
  std::size_t stopAfter{0};
};

Synthesis stream(const StreamRun& run)
{
  Synthesis result;
  run.engine.stream({.text = run.text,
                     .voice = run.voice,
                     .generation = {.temperature = 0.3F, .lsdSteps = 1},
                     .onAudio =
                         [&result](std::span<const float> samples) {
                           result.samples.insert(result.samples.end(), samples.begin(), samples.end());
                           ++result.calls;
                         },
                     .stopRequested = [&result, &run] { return run.stopAfter > 0 && result.calls >= run.stopAfter; }});
  return result;
}
}

TEST_CASE("Pocket's 24 kHz output is converted to the announced rate")
{
  constexpr int kSource = 24000;
  constexpr int kTarget = 44100;
  constexpr double kTone = 440.0;
  std::vector<float> tone(kSource);
  for (std::size_t index = 0; index < tone.size(); ++index)
    tone[index] = 0.5F * static_cast<float>(std::sin(2.0 * std::numbers::pi * kTone * static_cast<double>(index) / kSource));
  PcmRateConverter converter({.sourceRate = kSource, .targetRate = kTarget});
  std::vector<float> converted;
  for (std::size_t offset = 0; offset < tone.size(); offset += 1920) {
    const auto block = std::span<const float>(tone).subspan(offset, std::min<std::size_t>(1920, tone.size() - offset));
    const auto out = converter.process(block);
    converted.insert(converted.end(), out.begin(), out.end());
  }
  const auto tail = converter.flush();
  converted.insert(converted.end(), tail.begin(), tail.end());
  CHECK(converted.size() >= static_cast<std::size_t>(kTarget) - 64);
  CHECK(converted.size() <= static_cast<std::size_t>(kTarget) + 320);
  const std::vector<float> middle(converted.begin() + (kTarget / 4), converted.begin() + (3 * kTarget / 4));
  const auto crossings = static_cast<double>(zeroCrossings(middle));
  CHECK(crossings == doctest::Approx(kTone).epsilon(0.01));
  CHECK(PcmRateConverter({.sourceRate = kTarget, .targetRate = kTarget}).passthrough());
}

TEST_CASE("a reference recording name cannot leave the references directory")
{
  CHECK(isReferenceName("owner.wav"));
  CHECK(isReferenceName("voz_casa-2.wav"));
  CHECK_FALSE(isReferenceName("../owner.wav"));
  CHECK_FALSE(isReferenceName("dir/owner.wav"));
  CHECK_FALSE(isReferenceName(".hidden.wav"));
  CHECK_FALSE(isReferenceName("owner.mp3"));
  CHECK_FALSE(isReferenceName(""));
  const auto directory = std::filesystem::temp_directory_path() / "tts-pocket-references";
  std::filesystem::create_directories(directory);
  CHECK_FALSE(referencePath({.directory = directory, .name = "missing.wav", .targetRate = 24000}).has_value());
  std::filesystem::remove_all(directory);
}

TEST_CASE("the Pocket tokenizer reproduces the reference token ids")
{
  if (!provisioned("es-fast") || !provisioned("en")) {
    MESSAGE("Pocket models are not provisioned under " << pocketRoot().string() << "; skipped");
    return;
  }
  const auto spanish = UnigramTokenizer::load(pocketRoot() / "es-fast" / "tokenizer.json");
  CHECK(spanish.vocabularySize() == 4000);
  CHECK(spanish.encode("Hola, ¿cómo estás?") == std::vector<std::int64_t>{1878, 307, 261, 260, 198, 195, 2017, 436, 534, 263, 67});
  CHECK(spanish.encode("El Sr. Pérez llega a las dos.") == std::vector<std::int64_t>{346, 2527, 270, 3934, 681, 268, 282, 384, 270});
  CHECK(spanish.encode("  doble  espacio ") == std::vector<std::int64_t>{260, 260, 3142, 260, 288, 1397, 260});
  CHECK(spanish.encode("emoji 🙂 ok") == std::vector<std::int64_t>{2960, 1588, 260, 244, 163, 157, 134, 329, 111});
  const auto english = UnigramTokenizer::load(pocketRoot() / "en" / "tokenizer.json");
  CHECK(english.encode("Hello world.") == std::vector<std::int64_t>{2994, 578, 263});
  CHECK(english.encode("naïve café 3.5") == std::vector<std::int64_t>{913, 199, 179, 314, 331, 2250, 745, 260, 450, 263, 437});
}

TEST_CASE("the Pocket engine streams speech and honours cancellation")
{
  if (!provisioned("es-fast")) {
    MESSAGE("Pocket models are not provisioned under " << pocketRoot().string() << "; skipped");
    return;
  }
  Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "tts-pocket-test");
  PocketEngine engine({.env = env, .directory = pocketRoot() / "es-fast", .precision = "int8", .threads = 4});
  CHECK(engine.sampleRate() == 24000);
  CHECK(engine.tokenCount("Hola, ¿cómo estás?") == 11);
  const auto voice = engine.loadVoice(pocketRoot() / "es-fast" / "voices" / "lola.safetensors");
  CHECK(voice.length > 0);
  const auto full = stream({.engine = engine, .voice = voice, .text = "Hay una persona en la puerta principal.", .stopAfter = 0});
  const auto seconds = static_cast<double>(full.samples.size()) / 24000.0;
  CHECK(seconds > 1.0);
  CHECK(seconds < 8.0);
  CHECK(full.calls > 1);
  CHECK(std::ranges::all_of(full.samples, [](float sample) { return std::isfinite(sample); }));
  CHECK(std::ranges::max(full.samples) > 0.01F);
  const auto stopped = stream({.engine = engine, .voice = voice, .text = "Hay una persona en la puerta principal.", .stopAfter = 1});
  CHECK(stopped.calls == 1);
  CHECK(stopped.samples.size() < full.samples.size());
}

TEST_CASE("the service answers Pocket languages at the announced rate and falls back when Pocket is absent")
{
  if (!provisioned("es-fast") || !std::filesystem::exists(modelsRoot() / "onnx" / "tts.json")) {
    MESSAGE("Supertonic or Pocket models are not provisioned under " << modelsRoot().string() << "; skipped");
    return;
  }
  const auto empty = std::filesystem::temp_directory_path() / "tts-pocket-empty";
  std::filesystem::create_directories(empty);
  {
    const ScopedConfig config("[tts]\nmodels_dir = \"" + modelsRoot().string() +
                              "\"\nengine_es = \"pocket\"\nengine_en = \"pocket\"\nthreads = 4\n");
    auto& service = TtsService::instance();
    service.init();
    REQUIRE(service.isLoaded());
    CHECK(service.sampleRate() == 44100);
    std::vector<float> samples;
    service.synthesizeStream({.request = {.text = "Son las 14:30.", .lang = TtsLang::ES, .voiceId = "M3",
                                          .quality = TtsQuality::Auto, .speed = 1.0F},
                              .onChunk = [&samples](std::vector<float> chunk) {
                                samples.insert(samples.end(), chunk.begin(), chunk.end());
                              },
                              .stopRequested = {}});
    const auto seconds = static_cast<double>(samples.size()) / 44100.0;
    CHECK(seconds > 1.0);
    CHECK(seconds < 8.0);
    const auto engines = service.activeEngines();
    CHECK(std::ranges::find(engines, std::pair<std::string, std::string>{"es", "pocket"}) != engines.end());
    ConfigService::setString("tts.pocket_models_dir", empty.string());
    const auto fallback = service.activeEngines();
    CHECK(std::ranges::find(fallback, std::pair<std::string, std::string>{"es", "supertonic"}) != fallback.end());
    const auto supertonic = service.synthesize({.text = "Hola.", .lang = TtsLang::ES, .voiceId = "M3",
                                                .quality = TtsQuality::Low, .speed = 1.0F});
    CHECK_FALSE(supertonic.empty());
    service.shutdown();
  }
  std::filesystem::remove_all(empty);
}

TEST_CASE("a reference recording becomes a Pocket voice when the bundle carries the encoder")
{
  const auto* const configured = std::getenv("ARGUS_TEST_POCKET_CLONING_DIR");
  if (configured == nullptr) {
    MESSAGE("ARGUS_TEST_POCKET_CLONING_DIR is not set; skipped");
    return;
  }
  const std::filesystem::path bundle(configured);
  Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "tts-pocket-cloning-test");
  PocketEngine engine({.env = env, .directory = bundle, .precision = "int8", .threads = 4});
  REQUIRE(engine.canClone());
  const auto samples = loadReferenceAudio({.directory = bundle / "references", .name = "reference.wav", .targetRate = 24000});
  REQUIRE(samples.size() > 24000);
  const auto voice = engine.cloneVoice(samples);
  const auto frames = static_cast<std::int64_t>((std::min<std::size_t>(samples.size(), 240000) + 1920 + 1919) / 1920);
  CHECK(voice.length > 1);
  CHECK(voice.length <= frames + 1);
  const auto spoken = stream({.engine = engine, .voice = voice, .text = "Hola.", .stopAfter = 0});
  CHECK_FALSE(spoken.samples.empty());
  CHECK(std::ranges::all_of(spoken.samples, [](float sample) { return std::isfinite(sample); }));
}
