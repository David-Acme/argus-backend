#include "tts-service.hxx"

#include <feature/synthesis/infra/supertonic/onnx-utils.hxx>
#include <feature/synthesis/infra/supertonic/style.hxx>
#include <feature/synthesis/infra/supertonic/tts-engine.hxx>
#include <feature/synthesis/infra/supertonic/unicode-processor.hxx>

#include <drogon/drogon.h>
#include <config/config-service.hxx>
#include <runtime/blocking-task.hxx>
#include <runtime/hardware-profile.hxx>
#include <runtime/thread-budget.hxx>
#include <chrono>
#include <stdexcept>
#include <thread>

namespace
{
constexpr size_t kSynthCacheSlots = 64;
constexpr size_t kSynthCacheMaxChars = 300;

std::string modelsDir()
{
  const std::string dir = ConfigService::getString("tts.models_dir");
  return dir.empty() ? std::string("models/tts") : dir;
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
  std::lock_guard lifecycleLock(lifecycleMutex_);
  std::lock_guard lock(synthMutex_);
  if (loaded_)
    return;
  try {
    const std::string onnxDir = modelsDir() + "/onnx";

    auto nThreads = ThreadBudget::ttsThreads();
    if (const int cfg = ConfigService::getInt("tts.threads"); cfg > 0)
      nThreads = cfg;

    defaultSpeed_ = static_cast<float>(
        std::clamp(ConfigService::getDouble("tts.speed"), 0.7, 2.0));
    maxChunkLen_ =
        std::clamp(ConfigService::getInt("tts.max_chunk_len"), 30, 2000);

    const std::string q = ConfigService::getString("tts.quality");
    if (q == "high")
      defaultQuality_ = TtsQuality::High;
    else if (q == "medium")
      defaultQuality_ = TtsQuality::Medium;
    else if (q == "low")
      defaultQuality_ = TtsQuality::Low;
    else
      defaultQuality_ = TtsQuality::Auto;

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
  std::lock_guard lifecycleLock(lifecycleMutex_);
  stopping_.store(true);
  generation_.fetch_add(1);
  std::lock_guard lock(synthMutex_);
  engine_.reset();
  processor_.reset();
  voiceCache_.clear();
  synthCache_.clear();
  synthCacheNext_ = 0;
  loaded_ = false;
}

bool TtsService::isLoaded() const
{
  std::lock_guard lock(synthMutex_);
  return loaded_ && !stopping_.load();
}


std::vector<float> TtsService::synthesize(const TtsRequest& req)
{
  const auto generation = generation_.load();
  std::lock_guard lock(synthMutex_);
  if (stopping_.load() || generation != generation_.load() || !loaded_)
    throw std::runtime_error("TTS engine is not loaded");
  const auto& style = resolveVoice(req.voiceId);
  std::string langStr = langCode(req.lang);
  TtsQuality quality = resolveQuality(req);
  int steps = resolveSteps(quality);

  std::string key;
  const bool cacheable = req.text.size() <= kSynthCacheMaxChars;
  if (cacheable)
    key = req.voiceId + '\x1f' + langStr + '\x1f' + std::to_string(steps) +
          '\x1f' + std::to_string(req.speed) + '\x1f' + req.text;

  if (cacheable) {
    for (const auto& entry : synthCache_) {
      if (entry.key == key)
        return entry.samples;
    }
  }

  auto result = engine_->synthesize({.text = req.text,
                                     .lang = langStr,
                                     .style = style,
                                     .totalStep = steps,
                                     .speed = req.speed});

  if (cacheable && !result.wav.empty()) {
    if (synthCache_.size() < kSynthCacheSlots)
      synthCache_.push_back({.key = key, .samples = result.wav});
    else {
      synthCache_[synthCacheNext_] = {.key = key, .samples = result.wav};
      synthCacheNext_ = (synthCacheNext_ + 1) % kSynthCacheSlots;
    }
  }
  return result.wav;
}

void TtsService::synthesizeStream(const TtsRequest& req,
                                  TtsChunkCallback onChunk)
{
  synthesizeStream({.request = req,
                    .onChunk = std::move(onChunk),
                    .stopRequested = {}});
}

void TtsService::synthesizeStream(TtsStreamInput input)
{
  if (!input.onChunk)
    return;
  const auto generation = generation_.load();
  const auto stopped = [&] {
    return stopping_.load() || generation != generation_.load() ||
           (input.stopRequested && input.stopRequested());
  };
  const auto acquire = [&] {
    std::unique_lock lock(synthMutex_, std::defer_lock);
    while (!stopped()) {
      if (lock.try_lock_for(std::chrono::milliseconds(10))) {
        if (stopped())
          lock.unlock();
        return lock;
      }
    }
    return lock;
  };

  const auto& req = input.request;
  const std::string langStr = langCode(req.lang);
  std::vector<std::string> textList;
  int steps;
  {
    auto lock = acquire();
    if (!lock.owns_lock())
      return;
    if (!loaded_)
      throw std::runtime_error("TTS engine is not loaded");
    steps = resolveSteps(resolveQuality(req));
    const int maxLen =
        (req.lang == TtsLang::KO || req.lang == TtsLang::JA) ? 120 : maxChunkLen_;
    textList = chunkText(req.text, maxLen);
  }

  for (const auto& chunk : textList) {
    std::vector<float> audio;
    {
      auto lock = acquire();
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
  std::lock_guard lock(synthMutex_);
  if (stopping_.load() || !loaded_)
    return;
  voiceCache_[voiceId] = loadVoiceStyle(path);
}


int TtsService::sampleRate() const
{
  std::lock_guard lock(synthMutex_);
  return engine_ ? engine_->sampleRate() : 0;
}

std::vector<std::string> TtsService::availableVoices() const
{
  return {"M1", "M2", "M3", "M4", "M5", "F1", "F2", "F3", "F4", "F5"};
}

void TtsService::setDefaultQuality(TtsQuality q)
{
  std::lock_guard lock(synthMutex_);
  defaultQuality_ = q;
}

TtsQuality TtsService::defaultQuality() const
{
  std::lock_guard lock(synthMutex_);
  return defaultQuality_;
}

float TtsService::defaultSpeed() const
{
  std::lock_guard lock(synthMutex_);
  return defaultSpeed_;
}

const std::vector<std::string>& TtsService::supportedLangs()
{
  return supportedLangCodes();
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
  const auto it = voiceCache_.find(voiceId);
  if (it != voiceCache_.end())
    return *it->second;

  std::string path = modelsDir() + "/voice_styles/" + voiceId + ".json";
  auto style = loadVoiceStyle(path);

  auto [inserted, _] = voiceCache_.emplace(voiceId, std::move(style));
  return *inserted->second;
}
