#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <onnxruntime_cxx_api.h>
#include <optional>
#include <shared/wrapper/audio/sample-ring.hxx>
#include <vector>

struct VadConfig
{
  int sampleRate{16000};
  float threshold{0.45F};
  float negThreshold{0.25F};
  int minSpeechFrames{5};
  int minSilenceFrames{12};
  int maxTurnFrames{750};
  int preRollFrames{10};
  int minTurnMs{320};
  float minMeanProb{0.55F};
};

struct VadTurn
{
  std::vector<float> samples;
  int speechFrames{0};
  float meanProb{0.0F};
};

struct VadProcessInput
{
  const float* samples;
  int count{0};
};

class VadService
{
public:
  VadService();
  explicit VadService(const VadConfig& config);
  ~VadService();

  VadService(const VadService&) = delete;
  VadService& operator=(const VadService&) = delete;

  std::optional<VadTurn> process(const VadProcessInput& input);

  bool inSpeech() const;

  float lastProb() const;

  void reset();

  static bool isLoaded();

private:
  void runModel(float& prob);

  VadConfig cfg_;
  std::vector<float> state_;
  std::vector<float> context_;
  SampleRing pending_;
  std::vector<float> window_;
  std::vector<float> preRoll_;
  std::vector<float> buffer_;
  std::array<int64_t, 1> sampleRateInput_;
  std::array<int64_t, 2> inputShape_;
  std::array<int64_t, 3> stateShape_;
  std::array<int64_t, 1> srShape_;
  bool speech_{false};
  int startCounter_{0};
  int silenceCounter_{0};
  int frameCounter_{0};
  float speechProbSum_{0.0F};
  float lastProb_{0.0F};
};
