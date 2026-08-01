#include "tts-service.hxx"

#include "onnx-utils.hxx"
#include "style.hxx"
#include "tts-engine.hxx"
#include "unicode-processor.hxx"

#include <drogon/drogon.h>
#include <shared/wrapper/blocking-task/blocking-task.hxx>
#include <shared/wrapper/thread-budget/thread-budget.hxx>
#include <thread>

// --- Static members ---
std::unique_ptr<TtsEngine> TtsService::engine_;
std::unique_ptr<UnicodeProcessor> TtsService::processor_;
std::unordered_map<std::string, std::unique_ptr<Style>> TtsService::voiceCache_;
Ort::Env TtsService::env_{ORT_LOGGING_LEVEL_ERROR, "Argus-TTS"};
TtsQuality TtsService::defaultQuality_{TtsQuality::Auto};
bool TtsService::loaded_ = false;
std::mutex TtsService::synthMutex_;
std::mutex TtsService::voiceMutex_;

// --- Init / shutdown ---

void TtsService::init()
{
  try {
    const std::string onnxDir = "models/tts/onnx";
    const std::string voicesDir = "models/tts/voice_styles";

    Ort::SessionOptions opts;
    opts.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
    opts.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
    opts.SetIntraOpNumThreads(ThreadBudget::computeThreads());
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

    LOG_INFO << "TTS loaded: " << onnxDir;
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
  TtsQuality quality =
      (req.quality == TtsQuality::Auto) ? autoQuality(req.text) : req.quality;
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
  TtsQuality quality =
      (req.quality == TtsQuality::Auto) ? autoQuality(req.text) : req.quality;
  int steps = resolveSteps(quality);

  int maxLen = (req.lang == TtsLang::KO || req.lang == TtsLang::JA) ? 120 : 300;
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

int TtsService::resolveSteps(TtsQuality quality)
{
  switch (quality) {
    case TtsQuality::Low:
      return 5;
    case TtsQuality::Medium:
      return 8;
    case TtsQuality::High:
      return 10;
    case TtsQuality::Auto:
      return 8;
  }
  return 8;
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
