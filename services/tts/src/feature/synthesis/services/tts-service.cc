#include "tts-service.hxx"

#include <feature/synthesis/infra/pocket/pcm-rate-converter.hxx>
#include <feature/synthesis/infra/pocket/pocket-engine.hxx>
#include <feature/synthesis/infra/pocket/reference-audio.hxx>
#include <feature/synthesis/infra/supertonic/onnx-utils.hxx>
#include <feature/synthesis/infra/supertonic/style.hxx>
#include <feature/synthesis/infra/supertonic/tts-engine.hxx>
#include <feature/synthesis/infra/supertonic/unicode-processor.hxx>
#include <feature/synthesis/text/prosodic-chunker.hxx>
#include <feature/synthesis/text/text-normalizer.hxx>

#include <drogon/drogon.h>
#include <config/config-service.hxx>
#include <runtime/blocking-task.hxx>
#include <runtime/hardware-profile.hxx>
#include <runtime/thread-budget.hxx>
#include <chrono>
#include <stdexcept>
#include <thread>
#include <algorithm>
#include <cctype>

namespace
{
constexpr size_t kSynthCacheSlots = 64;
constexpr size_t kSynthCacheMaxChars = 300;
constexpr size_t kPocketVoiceCacheSlots = 32;
constexpr float kDefaultPocketTemperature = 0.3F;
constexpr int kDefaultPocketLsdSteps = 1;
constexpr const char* kDefaultSpanishVariant = "quality";
constexpr const char* kMalePocketVoice = "jean";

std::string modelsDir()
{
  const std::string dir = ConfigService::getString("tts.models_dir");
  return dir.empty() ? std::string("models/tts") : dir;
}

const char* engineKey(TtsLang lang)
{
  if (lang == TtsLang::ES)
    return "tts.engine_es";
  if (lang == TtsLang::EN)
    return "tts.engine_en";
  return nullptr;
}

const char* referenceKey(TtsLang lang)
{
  return lang == TtsLang::ES ? "tts.pocket_reference_es" : "tts.pocket_reference_en";
}

std::function<bool(const std::string&)> installedIn(const std::filesystem::path& directory)
{
  return [directory](const std::string& voice) {
    std::error_code error;
    return std::filesystem::is_regular_file(directory / "voices" / (voice + ".safetensors"), error);
  };
}

std::vector<std::string> pocketDirectories(TtsLang lang)
{
  if (lang == TtsLang::EN)
    return {"en"};
  if (TtsService::configuredPocketVariant(lang) == "quality")
    return {"es-quality", "es-fast"};
  return {"es-fast"};
}

}


TtsService::TtsService() = default;

TtsService::~TtsService()
{
  shutdown();
}

TtsService& TtsService::instance()
{
  static TtsService service;
  return service;
}

void TtsService::init()
{
  std::scoped_lock lifecycleLock(lifecycleMutex_);
  std::scoped_lock lock(synthMutex_);
  if (loaded_)
    return;
  try {
    const std::string onnxDir = modelsDir() + "/onnx";

    auto nThreads = ThreadBudget::ttsThreads();
    if (const int cfg = ConfigService::getInt("tts.threads"); cfg > 0)
      nThreads = cfg;
    threads_ = nThreads;

    loadDefaults();

    Ort::SessionOptions opts;
    opts.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    opts.SetIntraOpNumThreads(nThreads);
    opts.SetInterOpNumThreads(1);

    auto cfg = loadConfig(onnxDir);
    auto models =
        loadOnnxAll({.env = env_, .onnxDir = onnxDir, .opts = opts});
    processor_ = loadProcessor(onnxDir);

    Ort::MemoryInfo memoryInfo =
        Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    engine_ = std::make_unique<TtsEngine>(
        TtsEngine::Deps{.cfg = cfg,
                        .processor = processor_.get(),
                        .dp = std::move(models.dp),
                        .textEnc = std::move(models.textEnc),
                        .vectorEst = std::move(models.vectorEst),
                        .vocoder = std::move(models.vocoder),
                        .memoryInfo = std::move(memoryInfo)});

    loaded_ = true;
    stopping_.store(false);

    std::string qName = "auto";
    switch (defaultQuality_) {
      case TtsQuality::Low:
        qName = "low";
        break;
      case TtsQuality::Medium:
        qName = "medium";
        break;
      case TtsQuality::High:
        qName = "high";
        break;
      case TtsQuality::Auto:
        qName = "auto";
        break;
    }

    LOG_INFO << "TTS loaded: " << onnxDir << " (threads=" << nThreads
             << ", speed=" << defaultSpeed_ << ", quality=" << qName
             << ", max_chunk_len=" << maxChunkLen_ << ")";
    LOG_INFO << "TTS engines: es=" << engineName(configuredEngine(TtsLang::ES))
             << ", en=" << engineName(configuredEngine(TtsLang::EN))
             << " (Pocket models from " << pocketModelsDir().string() << ", loaded on first use)";
  }
  catch (const std::exception& e) {
    LOG_FATAL << "TTS init failed: " << e.what();
    engine_.reset();
    processor_.reset();
    loaded_ = false;
    stopping_.store(true);
  }
}

void TtsService::shutdown()
{
  std::scoped_lock lifecycleLock(lifecycleMutex_);
  stopping_.store(true);
  generation_.fetch_add(1);
  std::scoped_lock lock(synthMutex_);
  engine_.reset();
  processor_.reset();
  voiceCache_.clear();
  pocketVoices_.clear();
  pocketEngines_.clear();
  pocketFailures_.clear();
  warnings_.clear();
  synthCache_.clear();
  synthCacheNext_ = 0;
  loaded_ = false;
}

bool TtsService::isLoaded() const
{
  std::scoped_lock lock(synthMutex_);
  return loaded_ && !stopping_.load();
}


std::vector<float> TtsService::synthesize(const TtsRequest& req)
{
  if (configuredEngine(req.lang) == SpeechEngineKind::Pocket) {
    std::string key;
    {
      std::scoped_lock lock(synthMutex_);
      if (stopping_.load() || !loaded_)
        throw std::runtime_error("TTS engine is not loaded");
      if (req.text.size() <= kSynthCacheMaxChars) {
        key = cacheKey(req);
        for (const auto& entry : synthCache_) {
          if (entry.key == key)
            return entry.samples;
        }
      }
    }
    std::vector<float> samples;
    synthesizeStream({.request = req,
                      .onChunk = [&samples](std::vector<float> chunk) {
                        samples.insert(samples.end(), chunk.begin(), chunk.end());
                      },
                      .stopRequested = {}});
    if (!key.empty() && !samples.empty()) {
      std::scoped_lock lock(synthMutex_);
      remember(key, samples);
    }
    return samples;
  }

  const auto generation = generation_.load();
  const auto text = speechText(req);
  std::scoped_lock lock(synthMutex_);
  if (stopping_.load() || generation != generation_.load() || !loaded_)
    throw std::runtime_error("TTS engine is not loaded");
  const auto& style = resolveVoice(req.voiceId);
  std::string langStr = langCode(req.lang);
  TtsQuality quality = resolveQuality(req);
  int steps = resolveSteps(quality);

  std::string key;
  const bool cacheable = req.text.size() <= kSynthCacheMaxChars;
  if (cacheable)
    key = cacheKey(req);

  if (cacheable) {
    for (const auto& entry : synthCache_) {
      if (entry.key == key)
        return entry.samples;
    }
  }

  auto result = engine_->synthesize({.text = text,
                                     .lang = langStr,
                                     .style = style,
                                     .totalStep = steps,
                                     .speed = req.speed});

  if (cacheable && !result.wav.empty())
    remember(key, result.wav);
  return result.wav;
}

void TtsService::remember(const std::string& key, const std::vector<float>& samples)
{
  if (synthCache_.size() < kSynthCacheSlots)
    synthCache_.push_back({.key = key, .samples = samples});
  else {
    synthCache_[synthCacheNext_] = {.key = key, .samples = samples};
    synthCacheNext_ = (synthCacheNext_ + 1) % kSynthCacheSlots;
  }
}

std::string TtsService::cacheKey(const TtsRequest& req) const
{
  const auto text = speechText(req);
  const std::string langStr = langCode(req.lang);
  if (configuredEngine(req.lang) == SpeechEngineKind::Pocket) {
    std::string reference = ConfigService::getString(referenceKey(req.lang));
    const auto selection = pocketSelection(req.lang);
    const auto voice = selection ? resolvePocketVoice({.lang = req.lang,
                                                       .requestVoiceId = req.voiceId,
                                                       .installed = installedIn(selection->directory)})
                                 : std::nullopt;
    return "pocket\x1f" + (selection ? selection->variant : std::string()) + '\x1f' +
           (voice ? voice->voice : std::string()) + '\x1f' + reference + '\x1f' +
           std::to_string(configuredPocketTemperature()) + '\x1f' +
           std::to_string(configuredPocketLsdSteps()) + '\x1f' + langStr + '\x1f' + text;
  }
  const int steps = resolveSteps(resolveQuality(req));
  return req.voiceId + '\x1f' + langStr + '\x1f' + std::to_string(steps) + '\x1f' + std::to_string(req.speed) +
         '\x1f' + text;
}

void TtsService::synthesizeStream(const TtsRequest& req,
                                  TtsChunkCallback onChunk)
{
  synthesizeStream({.request = req,
                    .onChunk = std::move(onChunk),
                    .stopRequested = {}});
}

std::unique_lock<std::timed_mutex> TtsService::acquire(const std::function<bool()>& stopped)
{
  std::unique_lock lock(synthMutex_, std::defer_lock);
  while (!stopped()) {
    if (lock.try_lock_for(std::chrono::milliseconds(10))) {
      if (stopped())
        lock.unlock();
      return lock;
    }
  }
  return lock;
}

void TtsService::synthesizeStream(TtsStreamInput input)
{
  if (!input.onChunk)
    return;
  const auto generation = generation_.load();
  const std::function<bool()> stopped = [&] {
    return stopping_.load() || generation != generation_.load() ||
           (input.stopRequested && input.stopRequested());
  };

  const auto& req = input.request;
  const auto text = speechText(req);
  if (configuredEngine(req.lang) == SpeechEngineKind::Pocket &&
      streamPocket({.request = req, .text = text, .onChunk = input.onChunk, .stopped = stopped}))
    return;

  const std::string langStr = langCode(req.lang);
  std::vector<std::string> textList;
  int steps = 0;
  {
    auto lock = acquire(stopped);
    if (!lock.owns_lock())
      return;
    if (!loaded_)
      throw std::runtime_error("TTS engine is not loaded");
    steps = resolveSteps(resolveQuality(req));
    const int maxLen =
        (req.lang == TtsLang::KO || req.lang == TtsLang::JA) ? 120 : maxChunkLen_;
    textList = chunkProsodic({.text = text,
                              .maxUnits = static_cast<std::size_t>(maxLen),
                              .measure = codepointCount});
  }

  for (const auto& chunk : textList) {
    std::vector<float> audio;
    {
      auto lock = acquire(stopped);
      if (!lock.owns_lock())
        return;
      const auto& style = resolveVoice(req.voiceId);
      if (stopped())
        return;
      auto result = engine_->synthesize({.text = chunk,
                                         .lang = langStr,
                                         .style = style,
                                         .totalStep = steps,
                                         .speed = req.speed});
      audio = std::move(result.wav);
    }
    if (stopped())
      return;
    if (!audio.empty())
      input.onChunk(std::move(audio));
  }
}

bool TtsService::streamPocket(const PocketStreamJob& job)
{
  PocketEngine* engine = nullptr;
  std::shared_ptr<const PocketVoice> voice;
  std::vector<std::string> chunks;
  PocketGeneration generation;
  int targetRate = 0;
  {
    auto lock = acquire(job.stopped);
    if (!lock.owns_lock())
      return true;
    if (!loaded_)
      throw std::runtime_error("TTS engine is not loaded");
    const auto selection = pocketSelection(job.request.lang);
    if (!selection.has_value())
      return false;
    engine = pocketEngine(*selection);
    if (engine == nullptr)
      return false;
    voice = pocketVoice({.engine = *engine,
                         .selection = *selection,
                         .lang = job.request.lang,
                         .requestVoiceId = job.request.voiceId});
    if (!voice)
      return false;
    chunks = chunkProsodic({.text = job.text,
                            .maxUnits = PocketEngine::kMaxChunkTokens,
                            .measure = [engine](std::string_view text) { return engine->tokenCount(text); }});
    generation = {.temperature = configuredPocketTemperature(),
                  .lsdSteps = configuredPocketLsdSteps(),
                  .seed = static_cast<std::uint32_t>(std::max(0, ConfigService::getInt("tts.pocket_seed")))};
    targetRate = engine_ ? engine_->sampleRate() : engine->sampleRate();
  }

  PcmRateConverter converter({.sourceRate = engine->sampleRate(), .targetRate = targetRate});
  bool delivered = false;
  const auto deliver = [&](std::vector<float> samples) {
    if (samples.empty() || job.stopped())
      return;
    delivered = true;
    job.onChunk(std::move(samples));
  };
  for (const auto& chunk : chunks) {
    auto lock = acquire(job.stopped);
    if (!lock.owns_lock())
      return true;
    try {
      engine->stream({.text = chunk,
                      .voice = *voice,
                      .generation = generation,
                      .onAudio = [&](std::span<const float> pcm) { deliver(converter.process(pcm)); },
                      .stopRequested = job.stopped});
    }
    catch (const std::exception& error) {
      if (delivered)
        throw;
      warnOnce(std::string("Pocket synthesis failed, Supertonic answers instead: ") + error.what());
      return false;
    }
  }
  deliver(converter.flush());
  return true;
}

std::optional<PocketSelection> TtsService::pocketSelection(TtsLang lang) const
{
  if (configuredEngine(lang) != SpeechEngineKind::Pocket)
    return std::nullopt;
  const auto root = pocketModelsDir();
  for (const auto& variant : pocketDirectories(lang)) {
    const auto directory = root / variant;
    std::error_code error;
    if (!pocketFailures_.contains(variant) && std::filesystem::is_regular_file(directory / "bundle.json", error))
      return PocketSelection{.variant = variant, .directory = directory};
  }
  return std::nullopt;
}

PocketEngine* TtsService::pocketEngine(const PocketSelection& selection)
{
  if (const auto found = pocketEngines_.find(selection.variant); found != pocketEngines_.end())
    return found->second.get();
  try {
    const auto start = std::chrono::steady_clock::now();
    std::string precision = ConfigService::getString("tts.pocket_precision");
    if (precision != "fp32")
      precision = "int8";
    auto engine = std::make_unique<PocketEngine>(PocketEngineConfig{
        .env = env_, .directory = selection.directory, .precision = precision, .threads = threads_});
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
    LOG_INFO << "Pocket TTS loaded: " << selection.variant << " (" << precision << ", threads=" << threads_
             << ", voice cloning " << (engine->canClone() ? "available" : "not exported") << ", "
             << elapsed.count() << " ms)";
    auto* const loaded = engine.get();
    pocketEngines_.emplace(selection.variant, std::move(engine));
    return loaded;
  }
  catch (const std::exception& error) {
    pocketFailures_.insert(selection.variant);
    LOG_WARN << "Pocket TTS " << selection.variant << " failed to load, Supertonic answers instead: " << error.what();
    return nullptr;
  }
}

std::shared_ptr<const PocketVoice> TtsService::pocketVoice(const VoiceRequest& request)
{
  if (auto cloned = referenceVoice(request))
    return cloned;
  const auto resolution = resolvePocketVoice({.lang = request.lang,
                                               .requestVoiceId = request.requestVoiceId,
                                               .installed = installedIn(request.selection.directory)});
  if (!resolution) {
    warnOnce("No Pocket voice is installed for " + request.selection.variant + ", Supertonic answers instead");
    return nullptr;
  }
  if (resolution->fallback)
    warnOnce("Pocket voice " + configuredPocketVoice(request.lang) + " is not installed for " +
             request.selection.variant + ", " + resolution->voice + " answers instead");
  const auto& name = resolution->voice;
  const auto key = request.selection.variant + "/" + name;
  if (const auto found = pocketVoices_.find(key); found != pocketVoices_.end())
    return found->second;
  try {
    auto voice = std::make_shared<const PocketVoice>(
        request.engine.loadVoice(request.selection.directory / "voices" / (name + ".safetensors")));
    if (pocketVoices_.size() >= kPocketVoiceCacheSlots)
      pocketVoices_.clear();
    pocketVoices_.emplace(key, voice);
    return voice;
  }
  catch (const std::exception& error) {
    warnOnce("Pocket voice " + key + " is unavailable, Supertonic answers instead: " + error.what());
    return nullptr;
  }
}

std::shared_ptr<const PocketVoice> TtsService::referenceVoice(const VoiceRequest& request)
{
  const auto reference = ConfigService::getString(referenceKey(request.lang));
  if (reference.empty())
    return nullptr;
  if (!request.engine.canClone()) {
    warnOnce("Pocket " + request.selection.variant +
             " was exported without voice cloning; the predefined voice is used instead of " + reference);
    return nullptr;
  }
  const auto directory = pocketModelsDir() / "references";
  const auto path = referencePath({.directory = directory, .name = reference, .targetRate = 0});
  if (!path.has_value()) {
    warnOnce("Pocket reference " + reference + " is not a .wav file inside " + directory.string());
    return nullptr;
  }
  std::error_code error;
  const auto stamp = std::filesystem::last_write_time(*path, error).time_since_epoch().count();
  const auto key = request.selection.variant + "|reference|" + path->filename().string() + "|" +
                   std::to_string(stamp) + "|" + std::to_string(std::filesystem::file_size(*path, error));
  if (const auto found = pocketVoices_.find(key); found != pocketVoices_.end())
    return found->second;
  try {
    const auto samples = loadReferenceAudio(
        {.directory = directory, .name = reference, .targetRate = request.engine.sampleRate()});
    auto voice = std::make_shared<const PocketVoice>(request.engine.cloneVoice(samples));
    if (pocketVoices_.size() >= kPocketVoiceCacheSlots)
      pocketVoices_.clear();
    pocketVoices_.emplace(key, voice);
    LOG_INFO << "Pocket voice cloned from " << path->filename().string() << " for " << request.selection.variant;
    return voice;
  }
  catch (const std::exception& error) {
    warnOnce("Pocket reference " + reference + " could not be used: " + error.what());
    return nullptr;
  }
}

void TtsService::warnOnce(const std::string& message)
{
  if (warnings_.insert(message).second)
    LOG_WARN << message;
}

std::vector<std::pair<std::string, std::string>> TtsService::activeEngines() const
{
  std::scoped_lock lock(synthMutex_);
  std::vector<std::pair<std::string, std::string>> engines;
  engines.reserve(pocketLanguages().size());
  for (const auto& code : pocketLanguages()) {
    const auto lang = code == "es" ? TtsLang::ES : TtsLang::EN;
    const auto active = pocketSelection(lang).has_value() ? SpeechEngineKind::Pocket : SpeechEngineKind::Supertonic;
    engines.emplace_back(code, engineName(active));
  }
  return engines;
}

drogon::Task<std::vector<float>>
TtsService::synthesizeAsync(const TtsRequest& req)
{
  co_return co_await BlockingTask<std::vector<float>>(
      [this, req]() { return synthesize(req); });
}

drogon::Task<void> TtsService::synthesizeStreamAsync(const TtsRequest& req,
                                                     TtsChunkCallback onChunk)
{
  co_await BlockingTask<void>([this, req,
                               onChunk = std::move(onChunk)]() mutable {
    auto wrapped =
        [callback = std::move(onChunk)](const std::vector<float>& chunkPcm) {
          drogon::app().getLoop()->queueInLoop(
              [callback, chunkPcm]() { callback(chunkPcm); });
        };
    synthesizeStream(req, std::move(wrapped));
  });
  co_return;
}


void TtsService::loadVoice(const std::string& voiceId)
{
  std::string path = modelsDir() + "/voice_styles/" + voiceId + ".json";
  std::scoped_lock lock(synthMutex_);
  if (stopping_.load() || !loaded_)
    return;
  voiceCache_[voiceId] = loadVoiceStyle(path);
}


int TtsService::sampleRate() const
{
  std::scoped_lock lock(synthMutex_);
  return engine_ ? engine_->sampleRate() : 0;
}

std::vector<std::string> TtsService::availableVoices() const
{
  return {"M1", "M2", "M3", "M4", "M5", "F1", "F2", "F3", "F4", "F5"};
}

void TtsService::setDefaultQuality(TtsQuality q)
{
  std::scoped_lock lock(synthMutex_);
  defaultQuality_ = q;
}

TtsQuality TtsService::defaultQuality() const
{
  std::scoped_lock lock(synthMutex_);
  return defaultQuality_;
}

float TtsService::defaultSpeed() const
{
  std::scoped_lock lock(synthMutex_);
  return defaultSpeed_;
}

void TtsService::refreshDefaults()
{
  std::scoped_lock lock(synthMutex_);
  loadDefaults();
}

void TtsService::loadDefaults()
{
  defaultSpeed_ = static_cast<float>(std::clamp(ConfigService::getDouble("tts.speed"), 0.7, 2.0));
  maxChunkLen_ = std::clamp(ConfigService::getInt("tts.max_chunk_len"), 30, 2000);
  const std::string quality = ConfigService::getString("tts.quality");
  if (quality == "high")
    defaultQuality_ = TtsQuality::High;
  else if (quality == "medium")
    defaultQuality_ = TtsQuality::Medium;
  else if (quality == "low")
    defaultQuality_ = TtsQuality::Low;
  else
    defaultQuality_ = TtsQuality::Auto;
}

const std::vector<std::string>& TtsService::supportedLangs()
{
  return supportedLangCodes();
}

SpeechEngineKind TtsService::configuredEngine(TtsLang lang)
{
  const auto* key = engineKey(lang);
  if (key == nullptr)
    return SpeechEngineKind::Supertonic;
  return ConfigService::getString(key) == "supertonic" ? SpeechEngineKind::Supertonic : SpeechEngineKind::Pocket;
}

std::string TtsService::engineName(SpeechEngineKind kind)
{
  return kind == SpeechEngineKind::Pocket ? "pocket" : "supertonic";
}

const std::vector<std::string>& TtsService::pocketLanguages()
{
  static const std::vector<std::string> languages{"es", "en"};
  return languages;
}

const std::vector<std::string>& TtsService::pocketVoices(TtsLang lang)
{
  static const std::vector<std::string> spanish{"jean", "lola", "alba", "eve", "fantine", "giovanni", "marius", "javert", "michael"};
  static const std::vector<std::string> english{"jean", "alba", "eve", "jane", "mary", "marius", "javert", "michael", "george"};
  static const std::vector<std::string> none;
  if (lang == TtsLang::ES)
    return spanish;
  if (lang == TtsLang::EN)
    return english;
  return none;
}

std::string TtsService::configuredPocketVoice(TtsLang lang)
{
  const auto& voices = pocketVoices(lang);
  if (voices.empty())
    return {};
  const auto value = ConfigService::getString(lang == TtsLang::ES ? "tts.pocket_voice_es" : "tts.pocket_voice_en");
  return std::ranges::find(voices, value) != voices.end() ? value : voices.front();
}

std::string TtsService::fallbackPocketVoice(TtsLang lang)
{
  if (lang == TtsLang::ES)
    return "lola";
  if (lang == TtsLang::EN)
    return "alba";
  return {};
}

std::optional<PocketVoiceResolution> TtsService::resolvePocketVoice(const PocketVoiceChoice& choice)
{
  const auto configured = configuredPocketVoice(choice.lang);
  if (configured.empty() || !choice.installed)
    return std::nullopt;
  if (choice.installed(configured))
    return PocketVoiceResolution{.voice = configured, .fallback = false};
  const auto fallback = fallbackPocketVoice(choice.lang);
  std::vector<std::string> candidates{choice.requestVoiceId.starts_with('M') ? kMalePocketVoice : fallback, fallback};
  const auto& voices = pocketVoices(choice.lang);
  candidates.insert(candidates.end(), voices.begin(), voices.end());
  for (const auto& candidate : candidates)
    if (candidate != configured && choice.installed(candidate))
      return PocketVoiceResolution{.voice = candidate, .fallback = true};
  return std::nullopt;
}

std::string TtsService::configuredPocketVariant(TtsLang lang)
{
  if (lang != TtsLang::ES)
    return {};
  auto value = ConfigService::getString("tts.pocket_variant_es");
  if (value == "fast" || value == "quality")
    return value;
  return kDefaultSpanishVariant;
}

float TtsService::configuredPocketTemperature()
{
  if (!ConfigService::hasKey("tts.pocket_temperature"))
    return kDefaultPocketTemperature;
  return static_cast<float>(std::clamp(ConfigService::getDouble("tts.pocket_temperature"), 0.05, 1.0));
}

int TtsService::configuredPocketLsdSteps()
{
  if (!ConfigService::hasKey("tts.pocket_lsd_steps"))
    return kDefaultPocketLsdSteps;
  return std::clamp(ConfigService::getInt("tts.pocket_lsd_steps"), 1, 8);
}

bool TtsService::normalizationEnabled()
{
  return !ConfigService::hasKey("tts.normalize_text") || ConfigService::getBool("tts.normalize_text");
}

std::filesystem::path TtsService::modelsDirectory()
{
  return modelsDir();
}

std::filesystem::path TtsService::pocketModelsDir()
{
  auto configured = ConfigService::getString("tts.pocket_models_dir");
  if (!configured.empty())
    return configured;
  return std::filesystem::path(modelsDir()) / "pocket";
}

std::string TtsService::speechText(const TtsRequest& req)
{
  if (!normalizationEnabled())
    return req.text;
  return normalizeSpeechText(req.text, speechLanguage(langCode(req.lang)));
}


TtsQuality TtsService::resolveQuality(const TtsRequest& req) const
{
  if (req.quality != TtsQuality::Auto)
    return req.quality;
  if (defaultQuality_ != TtsQuality::Auto)
    return defaultQuality_;
  return autoQuality(req.text);
}

int TtsService::effectiveStepsCap()
{
  int cap = HardwareProbe::ttsStepsCap();
  if (const int pinned = ConfigService::getInt("tts.steps_cap"); pinned > 0)
    cap = pinned;
  return cap;
}

int TtsService::resolveSteps(TtsQuality quality)
{
  const int cap = effectiveStepsCap();
  int low = std::clamp(ConfigService::getInt("tts.steps_low"), 1, cap);
  int medium = std::clamp(ConfigService::getInt("tts.steps_medium"), 1, cap);
  int high = std::clamp(ConfigService::getInt("tts.steps_high"), 1, cap);
  switch (quality) {
    case TtsQuality::Low:
      return low;
    case TtsQuality::Medium:
      return medium;
    case TtsQuality::High:
      return high;
    case TtsQuality::Auto:
      return medium;
  }
  return medium;
}

TtsQuality TtsService::autoQuality(const std::string& text)
{
  if (text.empty())
    return TtsQuality::Medium;

  size_t len = text.size();
  int sentences = 0;

  for (size_t i = 0; i < text.size(); i++) {
    char c = text[i];
    if (c == '.' || c == '!' || c == '?')
      sentences++;
  }

  int score = 0;
  if (len > 300)
    score += 2;
  else if (len > 100)
    score += 1;

  if (sentences > 3)
    score += 1;

  if (score >= 2)
    return TtsQuality::High;
  return TtsQuality::Medium;
}

const Style& TtsService::resolveVoice(const std::string& voiceId)
{
  const bool safe =
      !voiceId.empty() && voiceId.size() <= 16 &&
      std::ranges::all_of(voiceId, [](char c) {
        return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_' ||
               c == '-';
      });
  if (!safe)
    throw std::invalid_argument("unknown voice style");
  const auto it = voiceCache_.find(voiceId);
  if (it != voiceCache_.end())
    return *it->second;

  std::string path = modelsDir() + "/voice_styles/" + voiceId + ".json";
  auto style = loadVoiceStyle(path);

  auto [inserted, _] = voiceCache_.emplace(voiceId, std::move(style));
  return *inserted->second;
}
