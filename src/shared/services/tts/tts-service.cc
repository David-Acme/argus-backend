#include "tts-service.hxx"

#include "onnx-utils.hxx"
#include "style.hxx"
#include "tts-engine.hxx"
#include "unicode-processor.hxx"

#include <drogon/drogon.h>
#include <shared/services/config-service/config-service.hxx>
#include <shared/wrapper/blocking-task/blocking-task.hxx>
#include <shared/wrapper/thread-budget/thread-budget.hxx>
#include <thread>

// --- Static members ---
std::unique_ptr<TtsEngine> TtsService::engine_;
std::unique_ptr<UnicodeProcessor> TtsService::processor_;
std::unordered_map<std::string, std::unique_ptr<Style>> TtsService::voiceCache_;
Ort::Env TtsService::env_{ORT_LOGGING_LEVEL_ERROR, "Argus-TTS"};
TtsQuality TtsService::defaultQuality_{TtsQuality::Auto};
float TtsService::defaultSpeed_{1.0F};
int TtsService::maxChunkLen_{300};
bool TtsService::loaded_ = false;
std::mutex TtsService::synthMutex_;
std::mutex TtsService::voiceMutex_;

// --- Init / shutdown ---

void TtsService::init()
{
  try {
    const std::string onnxDir = "models/tts/onnx";
    const std::string voicesDir = "models/tts/voice_styles";

    auto nThreads = ThreadBudget::computeThreads();
    if (const int cfg = ConfigService::getInt("tts.threads"); cfg > 0)
      nThreads = cfg;

    defaultSpeed_ = static_cast<float>(
        std::clamp(ConfigService::getDouble("tts.speed"), 0.7, 2.0));
    maxChunkLen_ =
        std::clamp(ConfigService::getInt("tts.max_chunk_len"), 30, 2000);

    // Default quality from config: "auto" (adaptive by text length),
    // "low", "medium" or "high". Absent/invalid => auto.
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
    auto models = loadOnnxAll(env_, onnxDir, opts);
    processor_ = loadProcessor(onnxDir);

    Ort::MemoryInfo memoryInfo =
        Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

    engine_ =
        std::make_unique<TtsEngine>(cfg, processor_.get(), std::move(models.dp),
                                    std::move(models.textEnc),
                                    std::move(models.vectorEst),
                                    std::move(models.vocoder),
                                    std::move(memoryInfo));

    loaded_ = true;

    std::string qName = "auto";
    switch (defaultQuality_) {
      case TtsQuality::Low: qName = "low"; break;
      case TtsQuality::Medium: qName = "medium"; break;
      case TtsQuality::High: qName = "high"; break;
      case TtsQuality::Auto: qName = "auto"; break;
    }

    LOG_INFO << "TTS loaded: " << onnxDir
             << " (threads=" << nThreads << ", speed=" << defaultSpeed_
             << ", quality=" << qName
             << ", max_chunk_len=" << maxChunkLen_ << ")";
  }
  catch (const std::exception& e) {
    LOG_FATAL << "TTS init failed: " << e.what();
    shutdown();
  }
}

void TtsService::shutdown()
{
  engine_.reset();
  processor_.reset();
  voiceCache_.clear();
  loaded_ = false;
}

bool TtsService::isLoaded()
{
  return loaded_;
}

// --- Synthesis ---

std::vector<float> TtsService::synthesize(const TtsRequest& req)
{
  const auto& style = resolveVoice(req.voiceId);
  std::string langStr = langCode(req.lang);
  TtsQuality quality = resolveQuality(req);
  int steps = resolveSteps(quality);

  std::lock_guard<std::mutex> lock(synthMutex_);
  auto result = engine_->synthesize(req.text, langStr, style, steps, req.speed);
  return result.wav;
}

void TtsService::synthesizeStream(const TtsRequest& req,
                                  TtsChunkCallback onChunk)
{
  if (!onChunk)
    return;

  const auto& style = resolveVoice(req.voiceId);
  std::string langStr = langCode(req.lang);
  TtsQuality quality = resolveQuality(req);
  int steps = resolveSteps(quality);

  int maxLen = (req.lang == TtsLang::KO || req.lang == TtsLang::JA) ? 120
                                                                    : maxChunkLen_;
  auto textList = chunkText(req.text, maxLen);

  std::lock_guard<std::mutex> lock(synthMutex_);
  for (const auto& chunk : textList) {
    auto result = engine_->synthesize(chunk, langStr, style, steps, req.speed);
    if (!result.wav.empty())
      onChunk(result.wav);
  }
}

drogon::Task<std::vector<float>>
TtsService::synthesizeAsync(const TtsRequest& req)
{
  co_return co_await BlockingTask<std::vector<float>>(
      [req]() { return TtsService::synthesize(req); });
}

drogon::Task<void> TtsService::synthesizeStreamAsync(const TtsRequest& req,
                                                     TtsChunkCallback onChunk)
{
  co_await BlockingTask<void>([req, onChunk = std::move(onChunk)]() mutable {
    auto wrapped = [callback = std::move(onChunk)](
                       const std::vector<float>& chunkPcm) {
      drogon::app().getLoop()->queueInLoop(
          [callback, chunkPcm]() { callback(chunkPcm); });
    };
    TtsService::synthesizeStream(req, std::move(wrapped));
  });
  co_return;
}

// --- Voice cache ---

void TtsService::loadVoice(const std::string& voiceId)
{
  std::string path = "models/tts/voice_styles/" + voiceId + ".json";
  std::lock_guard<std::mutex> lock(voiceMutex_);
  voiceCache_[voiceId] = loadVoiceStyle(path);
}

// --- Getters ---

int TtsService::sampleRate()
{
  return engine_->sampleRate();
}

std::vector<std::string> TtsService::availableVoices()
{
  return {"M1", "M2", "M3", "M4", "M5", "F1", "F2", "F3", "F4", "F5"};
}

void TtsService::setDefaultQuality(TtsQuality q)
{
  defaultQuality_ = q;
}

TtsQuality TtsService::defaultQuality()
{
  return defaultQuality_;
}

float TtsService::defaultSpeed()
{
  return defaultSpeed_;
}

// --- Static utils ---

void TtsService::writeWav(const std::string& path,
                          const std::vector<float>& pcm, int sampleRate)
{
  ::writeWav(path, pcm, sampleRate);
}

const std::vector<std::string>& TtsService::supportedLangs()
{
  return supportedLangCodes();
}

// --- Private ---

TtsQuality TtsService::resolveQuality(const TtsRequest& req)
{
  if (req.quality != TtsQuality::Auto)
    return req.quality;
  if (defaultQuality_ != TtsQuality::Auto)
    return defaultQuality_;
  return autoQuality(req.text);
}

int TtsService::resolveSteps(TtsQuality quality)
{
  int low = std::clamp(ConfigService::getInt("tts.steps_low"), 1, 50);
  int medium = std::clamp(ConfigService::getInt("tts.steps_medium"), 1, 50);
  int high = std::clamp(ConfigService::getInt("tts.steps_high"), 1, 50);
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

  if (score >= 3)
    return TtsQuality::High;
  if (score >= 1)
    return TtsQuality::Medium;
  return TtsQuality::Low;
}

const Style& TtsService::resolveVoice(const std::string& voiceId)
{
  {
    std::lock_guard<std::mutex> lock(voiceMutex_);
    auto it = voiceCache_.find(voiceId);
    if (it != voiceCache_.end()) {
      return *it->second;
    }
  }

  std::string path = "models/tts/voice_styles/" + voiceId + ".json";
  auto style = loadVoiceStyle(path);

  std::lock_guard<std::mutex> lock(voiceMutex_);
  auto [inserted, _] = voiceCache_.emplace(voiceId, std::move(style));
  return *inserted->second;
}
