#pragma once

#include <atomic>
#include <cstdint>
#include <drogon/utils/coroutine.h>
#include <feature/voiceprint/services/audio/speech-quality.hxx>
#include <feature/voiceprint/services/audio/voice-audio.hxx>
#include <memory>
#include <optional>
#include <semaphore>
#include <shared_mutex>
#include <span>
#include <string>
#include <vector>

struct SherpaOnnxSpeakerEmbeddingExtractor;

enum class VoiceAnalysisStatus : uint8_t
{
  Ok = 0,
  Unavailable,
  Invalid,
  TooShort,
  TooNoisy,
  Clipped
};

struct VoiceAnalysisInput
{
  EncodedVoice voice;
  SpeechRequirement requirement;
  bool extractEmbedding{true};
};

struct VoiceAnalysis
{
  VoiceAnalysisStatus status{VoiceAnalysisStatus::Invalid};
  SpeechQuality quality;
  std::vector<float> embedding;
};

class SpeakerEmbeddingService
{
public:
  SpeakerEmbeddingService();
  ~SpeakerEmbeddingService();

  SpeakerEmbeddingService(const SpeakerEmbeddingService&) = delete;
  SpeakerEmbeddingService& operator=(const SpeakerEmbeddingService&) = delete;

  static SpeakerEmbeddingService& instance();

  bool init(const std::string& modelPath);
  void disable();
  void shutdown();

  [[nodiscard]] bool isLoaded() const;
  [[nodiscard]] int dims() const;
  [[nodiscard]] std::string modelId() const;

  [[nodiscard]] static std::string modelIdOf(const std::string& modelPath);

  [[nodiscard]] std::optional<std::vector<float>>
  embed(std::span<const float> samples);

  [[nodiscard]] VoiceAnalysis analyze(const VoiceAnalysisInput& input);

  drogon::Task<VoiceAnalysis> analyzeAsync(VoiceAnalysisInput input);

private:
  using ExtractorDeleter = void (*)(const SherpaOnnxSpeakerEmbeddingExtractor*);

  std::counting_semaphore<8> slots_{0};
  std::atomic<bool> disabled_{false};
  mutable std::shared_mutex lifetime_;
  std::unique_ptr<const SherpaOnnxSpeakerEmbeddingExtractor, ExtractorDeleter>
      extractor_;
  int dims_{0};
  std::string modelId_;
};
