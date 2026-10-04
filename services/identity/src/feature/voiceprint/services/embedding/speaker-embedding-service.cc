#include "speaker-embedding-service.hxx"

#include <drogon/drogon.h>
#include <feature/voiceprint/services/embedding/voice-vector.hxx>
#include <filesystem>
#include <mutex>
#include <runtime/blocking-task.hxx>
#include <runtime/thread-budget.hxx>
#include <sherpa-onnx/c-api/c-api.h>

namespace
{

using StreamHandle = std::unique_ptr<const SherpaOnnxOnlineStream,
                                     void (*)(const SherpaOnnxOnlineStream*)>;
using EmbeddingHandle = std::unique_ptr<const float, void (*)(const float*)>;

VoiceAnalysisStatus statusOf(SpeechProblem problem)
{
  switch (problem) {
    case SpeechProblem::TooShort:
      return VoiceAnalysisStatus::TooShort;
    case SpeechProblem::TooNoisy:
      return VoiceAnalysisStatus::TooNoisy;
    case SpeechProblem::Clipped:
      return VoiceAnalysisStatus::Clipped;
    case SpeechProblem::None:
      break;
  }
  return VoiceAnalysisStatus::Ok;
}

class SlotGuard
{
public:
  explicit SlotGuard(std::counting_semaphore<8>& slots) : slots_(slots)
  {
    slots_.acquire();
  }
  ~SlotGuard() { slots_.release(); }

  SlotGuard(const SlotGuard&) = delete;
  SlotGuard& operator=(const SlotGuard&) = delete;

private:
  std::counting_semaphore<8>& slots_;
};

}

SpeakerEmbeddingService::SpeakerEmbeddingService()
    : extractor_(nullptr, &SherpaOnnxDestroySpeakerEmbeddingExtractor)
{
}

SpeakerEmbeddingService::~SpeakerEmbeddingService()
{
  shutdown();
}

SpeakerEmbeddingService& SpeakerEmbeddingService::instance()
{
  static SpeakerEmbeddingService service;
  return service;
}

std::string SpeakerEmbeddingService::modelIdOf(const std::string& modelPath)
{
  return std::filesystem::path(modelPath).stem().string();
}

bool SpeakerEmbeddingService::init(const std::string& modelPath)
{
  {
    const std::unique_lock lock(lifetime_);
    modelId_ = modelIdOf(modelPath);
    std::error_code error;
    if (!std::filesystem::is_regular_file(modelPath, error)) {
      LOG_WARN << "SpeakerEmbeddingService: model " << modelPath
               << " is missing; voice recognition disabled";
    }
    else {
      SherpaOnnxSpeakerEmbeddingExtractorConfig config{};
      config.model = modelPath.c_str();
      config.num_threads = ThreadBudget::lightThreads();
      config.debug = 0;
      config.provider = "cpu";
      extractor_.reset(SherpaOnnxCreateSpeakerEmbeddingExtractor(&config));
      if (extractor_)
        dims_ = SherpaOnnxSpeakerEmbeddingExtractorDim(extractor_.get());
      else
        LOG_WARN << "SpeakerEmbeddingService: model " << modelPath
                 << " failed to load; voice recognition disabled";
    }
  }
  if (!isLoaded()) {
    disable();
    return false;
  }
  slots_.release(ThreadBudget::inferenceSlots());
  LOG_INFO << "SpeakerEmbeddingService: " << modelId_
           << " loaded (dims=" << dims_
           << ", threads=" << ThreadBudget::lightThreads() << ")";
  return true;
}

void SpeakerEmbeddingService::disable()
{
  if (disabled_.exchange(true))
    return;
  slots_.release(ThreadBudget::inferenceSlots());
}

void SpeakerEmbeddingService::shutdown()
{
  const std::unique_lock lock(lifetime_);
  extractor_.reset();
}

bool SpeakerEmbeddingService::isLoaded() const
{
  const std::shared_lock lock(lifetime_);
  return extractor_ != nullptr && !disabled_.load();
}

int SpeakerEmbeddingService::dims() const
{
  const std::shared_lock lock(lifetime_);
  return dims_;
}

std::string SpeakerEmbeddingService::modelId() const
{
  const std::shared_lock lock(lifetime_);
  return modelId_;
}

std::optional<std::vector<float>>
SpeakerEmbeddingService::embed(std::span<const float> samples)
{
  if (disabled_.load() || samples.empty())
    return std::nullopt;
  const SlotGuard slot(slots_);
  const std::shared_lock lock(lifetime_);
  if (!extractor_ || disabled_.load())
    return std::nullopt;

  const StreamHandle stream(SherpaOnnxSpeakerEmbeddingExtractorCreateStream(
                                extractor_.get()),
                            &SherpaOnnxDestroyOnlineStream);
  if (!stream)
    return std::nullopt;
  SherpaOnnxOnlineStreamAcceptWaveform(stream.get(), voice_audio::kModelRate,
                                       samples.data(),
                                       static_cast<int32_t>(samples.size()));
  SherpaOnnxOnlineStreamInputFinished(stream.get());
  if (SherpaOnnxSpeakerEmbeddingExtractorIsReady(extractor_.get(),
                                                 stream.get()) == 0)
    return std::nullopt;

  const EmbeddingHandle embedding(
      SherpaOnnxSpeakerEmbeddingExtractorComputeEmbedding(extractor_.get(),
                                                          stream.get()),
      &SherpaOnnxSpeakerEmbeddingExtractorDestroyEmbedding);
  if (!embedding || dims_ <= 0)
    return std::nullopt;
  return voice_vector::normalized(
      std::span<const float>(embedding.get(), static_cast<size_t>(dims_)));
}

std::optional<float> SpeakerEmbeddingService::halvesScore(const HalvesInput& input)
{
  const auto minSamples = static_cast<size_t>(
      input.minSeconds * static_cast<float>(voice_audio::kModelRate));
  if (minSamples == 0 || input.speech.size() < 2 * minSamples)
    return std::nullopt;
  const size_t middle = input.speech.size() / 2;
  const auto first = embed(input.speech.first(middle));
  const auto second = embed(input.speech.subspan(middle));
  if (!first || !second)
    return std::nullopt;
  return voice_vector::cosine(*first, *second);
}

VoiceAnalysis SpeakerEmbeddingService::analyze(const VoiceAnalysisInput& input)
{
  VoiceAnalysis analysis;
  if (input.extractEmbedding && !isLoaded()) {
    analysis.status = VoiceAnalysisStatus::Unavailable;
    return analysis;
  }
  const auto clip = voice_audio::decode(input.voice);
  if (!clip) {
    analysis.status = VoiceAnalysisStatus::Invalid;
    return analysis;
  }
  const std::vector<float> samples = voice_audio::toModelRate(*clip);
  analysis.quality = speech_quality::measure(samples);
  analysis.status =
      statusOf(speech_quality::judge(analysis.quality, input.requirement));
  if (analysis.status != VoiceAnalysisStatus::Ok || !input.extractEmbedding)
    return analysis;

  const auto speech = speech_quality::speechSpan(samples, analysis.quality);
  auto embedding = embed(speech);
  if (!embedding) {
    analysis.status = isLoaded() ? VoiceAnalysisStatus::Invalid
                                 : VoiceAnalysisStatus::Unavailable;
    return analysis;
  }
  analysis.embedding = std::move(*embedding);
  if (input.halvesMinSeconds)
    analysis.halvesScore = halvesScore(
        {.speech = speech, .minSeconds = *input.halvesMinSeconds});
  return analysis;
}

drogon::Task<VoiceAnalysis>
SpeakerEmbeddingService::analyzeAsync(VoiceAnalysisInput input)
{
  auto shared = std::make_shared<const VoiceAnalysisInput>(std::move(input));
  co_return co_await BlockingTask<VoiceAnalysis>(
      [this, shared]() { return analyze(*shared); }, BlockingLane::Heavy);
}
