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

PocketVoiceResolution resolvedVoice(const PocketVoiceChoice& choice)
{
  return TtsService::resolvePocketVoice(choice).value_or(PocketVoiceResolution{.voice = "none", .fallback = false});
}

void touch(const std::filesystem::path& file)
{
  std::filesystem::create_directories(file.parent_path());
  std::ofstream(file) << "{}";
}

std::filesystem::path fakePocketTree()
{
  const auto root = std::filesystem::temp_directory_path() / "tts-pocket-warm";
  std::filesystem::remove_all(root);
  for (const auto* file : {"es-quality/bundle.json", "es-quality/voices/jean.safetensors",
                           "es-quality/voices/lola.safetensors", "es-fast/bundle.json",
                           "es-fast/voices/lola.safetensors", "en/bundle.json", "en/voices/alba.safetensors"})
    touch(root / file);
  return root;
}

std::string pocketConfig(const std::filesystem::path& root, const std::string& keys)
{
  return "[tts]\npocket_models_dir = \"" + root.string() + "\"\n" + keys;
}

std::vector<std::string> warmUpPlan(const std::filesystem::path& root)
{
  std::vector<std::string> described;
  for (const auto& target : TtsService::instance().warmUpPlan()) {
    std::string line = std::string(langCode(target.lang)) + " " + TtsService::engineName(target.engine);
    if (target.engine == SpeechEngineKind::Pocket) {
      CHECK(target.pocket.directory == root / target.pocket.variant);
      line += " " + target.pocket.variant;
    }
    described.push_back(line + " " + target.voice);
  }
  return described;
}

Synthesis stream(const StreamRun& run)
{
  Synthesis result;
  run.engine.stream({.text = run.text,
                     .voice = run.voice,
                     .generation = {.temperature = 0.3F, .lsdSteps = 1, .seed = 0},
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

TEST_CASE("jean answers every request id by default in both languages, and the quality model is the Spanish default")
{
  const ScopedConfig config("");
  CHECK(TtsService::configuredPocketVariant(TtsLang::ES) == "quality");
  const auto everything = [](const std::string&) { return true; };
  for (const auto lang : {TtsLang::ES, TtsLang::EN}) {
    CHECK(TtsService::configuredPocketVoice(lang) == "jean");
    for (const auto* id : {"M1", "M2", "M3", "M4", "M5", "F1", "F2", "F3", "F4", "F5"}) {
      const auto resolved = resolvedVoice({.lang = lang, .requestVoiceId = id, .installed = everything});
      CHECK(resolved.voice == "jean");
      CHECK_FALSE(resolved.fallback);
    }
  }
}

TEST_CASE("without jean installed the Commons voice of each language answers")
{
  const ScopedConfig config("");
  const auto withoutJean = [](const std::string& voice) { return voice != "jean"; };
  for (const auto* id : {"M3", "F1"}) {
    const auto spanish = resolvedVoice({.lang = TtsLang::ES, .requestVoiceId = id, .installed = withoutJean});
    CHECK(spanish.voice == "lola");
    CHECK(spanish.fallback);
    const auto english = resolvedVoice({.lang = TtsLang::EN, .requestVoiceId = id, .installed = withoutJean});
    CHECK(english.voice == "alba");
    CHECK(english.fallback);
  }
  const auto onlyGiovanni = [](const std::string& voice) { return voice == "giovanni"; };
  const auto last = resolvedVoice({.lang = TtsLang::ES, .requestVoiceId = "M3", .installed = onlyGiovanni});
  CHECK(last.voice == "giovanni");
  const auto nothing = [](const std::string&) { return false; };
  CHECK_FALSE(TtsService::resolvePocketVoice({.lang = TtsLang::ES, .requestVoiceId = "M3", .installed = nothing}).has_value());
  CHECK_FALSE(TtsService::resolvePocketVoice({.lang = TtsLang::FR, .requestVoiceId = "M3", .installed = withoutJean}).has_value());
}

TEST_CASE("the owner's voice choice wins, and a missing one keeps the requested gender")
{
  const ScopedConfig config("[tts]\npocket_voice_es = \"lola\"\npocket_voice_en = \"michael\"\n");
  const auto everything = [](const std::string&) { return true; };
  for (const auto* id : {"M3", "F2"})
    CHECK(resolvedVoice({.lang = TtsLang::ES, .requestVoiceId = id, .installed = everything}).voice == "lola");
  const auto withoutMichael = [](const std::string& voice) { return voice != "michael"; };
  const auto male = resolvedVoice({.lang = TtsLang::EN, .requestVoiceId = "M1", .installed = withoutMichael});
  CHECK(male.voice == "jean");
  CHECK(male.fallback);
  const auto female = resolvedVoice({.lang = TtsLang::EN, .requestVoiceId = "F4", .installed = withoutMichael});
  CHECK(female.voice == "alba");
  const ScopedConfig unknown("[tts]\npocket_voice_es = \"cosette\"\n");
  CHECK(TtsService::configuredPocketVoice(TtsLang::ES) == "jean");
}

TEST_CASE("the boot warm-up names exactly the configured engine, variant and voice of each language")
{
  const auto root = fakePocketTree();
  {
    const ScopedConfig config(pocketConfig(root, "engine_es = \"pocket\"\nengine_en = \"pocket\"\n"
                                                 "pocket_variant_es = \"quality\"\npocket_voice_es = \"jean\"\n"
                                                 "pocket_voice_en = \"jean\"\n"));
    CHECK(warmUpPlan(root) == std::vector<std::string>{"es pocket es-quality jean", "en pocket en alba"});
  }
  {
    const ScopedConfig config(pocketConfig(root, "engine_es = \"pocket\"\nengine_en = \"supertonic\"\n"
                                                 "pocket_variant_es = \"fast\"\npocket_voice_es = \"lola\"\n"));
    CHECK(warmUpPlan(root) == std::vector<std::string>{"es pocket es-fast lola", "en supertonic M3"});
  }
  {
    const ScopedConfig config(pocketConfig(root, "engine_es = \"supertonic\"\nengine_en = \"supertonic\"\n"));
    CHECK(warmUpPlan(root) == std::vector<std::string>{"es supertonic M3", "en supertonic M3"});
  }
  std::filesystem::remove(root / "es-quality" / "bundle.json");
  {
    const ScopedConfig config(pocketConfig(root, "pocket_variant_es = \"quality\"\npocket_voice_es = \"giovanni\"\n"));
    CHECK(warmUpPlan(root) == std::vector<std::string>{"es pocket es-fast lola", "en pocket en alba"});
  }
  std::filesystem::remove(root / "es-fast" / "voices" / "lola.safetensors");
  std::filesystem::remove(root / "en" / "bundle.json");
  {
    const ScopedConfig config(pocketConfig(root, ""));
    CHECK(warmUpPlan(root) == std::vector<std::string>{"es supertonic M3", "en supertonic M3"});
  }
  std::filesystem::remove_all(root);
}

TEST_CASE("the Pocket tokenizer reproduces the reference token ids" *
          doctest::skip(!provisioned("es-fast") || !provisioned("en")))
{
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

TEST_CASE("the Pocket engine streams speech and honours cancellation" *
          doctest::skip(!provisioned("es-fast")))
{
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

TEST_CASE("a fixed seed makes Pocket speech reproducible" *
          doctest::skip(!provisioned("es-fast")))
{
  Ort::Env env(ORT_LOGGING_LEVEL_ERROR, "tts-pocket-seed-test");
  PocketEngine engine({.env = env, .directory = pocketRoot() / "es-fast", .precision = "int8", .threads = 4});
  const auto voice = engine.loadVoice(pocketRoot() / "es-fast" / "voices" / "lola.safetensors");
  const auto speak = [&engine, &voice](std::uint32_t seed) {
    std::vector<float> samples;
    engine.stream({.text = "Buenas tardes.",
                   .voice = voice,
                   .generation = {.temperature = 0.3F, .lsdSteps = 1, .seed = seed},
                   .onAudio = [&samples](std::span<const float> pcm) { samples.insert(samples.end(), pcm.begin(), pcm.end()); },
                   .stopRequested = {}});
    return samples;
  };
  const auto first = speak(7);
  REQUIRE_FALSE(first.empty());
  CHECK(speak(7) == first);
  CHECK(speak(8) != first);
}

TEST_CASE("the service answers Pocket languages at the announced rate and falls back when Pocket is absent" *
          doctest::skip(!provisioned("es-fast") || !std::filesystem::exists(modelsRoot() / "onnx" / "tts.json")))
{
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

TEST_CASE("a warm-up that cannot load a Pocket model leaves the lazy fallback where it was" *
          doctest::skip(!std::filesystem::exists(modelsRoot() / "onnx" / "tts.json")))
{
  const auto root = fakePocketTree();
  {
    const ScopedConfig config(pocketConfig(root, "models_dir = \"" + modelsRoot().string() +
                                                     "\"\nengine_es = \"pocket\"\nengine_en = \"supertonic\"\nthreads = 4\n"));
    auto& service = TtsService::instance();
    service.warmUp();
    CHECK(warmUpPlan(root) == std::vector<std::string>{"es pocket es-quality jean", "en supertonic M3"});
    service.init();
    REQUIRE(service.isLoaded());
    service.warmUp();
    CHECK(warmUpPlan(root) == std::vector<std::string>{"es pocket es-fast lola", "en supertonic M3"});
    service.warmUp();
    CHECK(warmUpPlan(root) == std::vector<std::string>{"es supertonic M3", "en supertonic M3"});
    const auto engines = service.activeEngines();
    CHECK(std::ranges::find(engines, std::pair<std::string, std::string>{"es", "supertonic"}) != engines.end());
    const auto speech = service.synthesize({.text = "Hola.", .lang = TtsLang::ES, .voiceId = "M3",
                                            .quality = TtsQuality::Low, .speed = 1.0F});
    CHECK_FALSE(speech.empty());
    service.shutdown();
    CHECK(warmUpPlan(root) == std::vector<std::string>{"es pocket es-quality jean", "en supertonic M3"});
  }
  std::filesystem::remove_all(root);
}

TEST_CASE("a reference recording becomes a Pocket voice when the bundle carries the encoder" *
          doctest::skip(std::getenv("ARGUS_TEST_POCKET_CLONING_DIR") == nullptr))
{
  const auto* const configured = std::getenv("ARGUS_TEST_POCKET_CLONING_DIR");
  REQUIRE(configured != nullptr);
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
