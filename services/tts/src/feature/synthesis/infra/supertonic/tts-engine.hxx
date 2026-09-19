#pragma once

#include "onnx-utils.hxx"

#include <cstdint>
#include <memory>
#include <onnxruntime_cxx_api.h>
#include <random>
#include <string>
#include <vector>

class UnicodeProcessor;
class Style;

class TtsEngine
{
public:
  struct Deps
  {
    const Config& cfg;
    UnicodeProcessor* processor{nullptr};
    std::unique_ptr<Ort::Session> dp;
    std::unique_ptr<Ort::Session> textEnc;
    std::unique_ptr<Ort::Session> vectorEst;
    std::unique_ptr<Ort::Session> vocoder;
    Ort::MemoryInfo&& memoryInfo;
  };

  explicit TtsEngine(Deps deps);
  ~TtsEngine() = default;

  TtsEngine(const TtsEngine&) = delete;
  TtsEngine& operator=(const TtsEngine&) = delete;

  struct Result
  {
    std::vector<float> wav;
    std::vector<float> duration;
  };

  struct SynthesizeInput
  {
    const std::string& text;
    const std::string& lang;
    const Style& style;
    int totalStep{0};
    float speed{1.0F};
  };

  Result synthesize(const SynthesizeInput& input) const;

  int sampleRate() const { return sampleRate_; }

private:
  struct InferInput
  {
    const std::vector<std::string>& textList;
    const std::vector<std::string>& langList;
    const Style& style;
    int totalStep{0};
    float speed{1.0F};
  };

  Result infer(const InferInput& input) const;

  struct LatentSample
  {
    std::vector<std::vector<std::vector<float>>> noisyLatent;
    std::vector<std::vector<std::vector<float>>> latentMask;
  };

  LatentSample sampleNoisyLatent(const std::vector<float>& duration) const;

  Config cfg_;
  UnicodeProcessor* processor_;
  std::unique_ptr<Ort::Session> dp_;
  std::unique_ptr<Ort::Session> textEnc_;
  std::unique_ptr<Ort::Session> vectorEst_;
  std::unique_ptr<Ort::Session> vocoder_;
  mutable Ort::MemoryInfo memoryInfo_;

  int sampleRate_;
  int baseChunkSize_;
  int edgeSilenceMs_{40};
  int joinSilenceMs_{90};
  int chunkCompressFactor_;
  int ldim_;

  mutable std::vector<std::vector<float>> tensorFloats_;
  mutable std::vector<std::vector<int64_t>> tensorInts_;

  mutable std::mt19937 rng_{std::random_device{}()};
  mutable std::normal_distribution<float> noiseDist_{0.0f, 1.0f};
};
