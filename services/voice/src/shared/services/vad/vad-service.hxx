#pragma once

#include <cstdint>
#include <memory>
#include <onnxruntime_cxx_api.h>
#include <optional>
#include <shared/wrapper/audio/sample-ring.hxx>
#include <span>
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
  float bargeThreshold{0.7F};
  int bargeMinFrames{8};
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

struct VadListenInput
{
  const float* samples;
  int count{0};
  bool armed{false};
};

class VadModel
{
public:
  virtual ~VadModel() = default;

  virtual float probability(std::span<const float> window) = 0;
  virtual void reset() = 0;
};

std::unique_ptr<VadModel> makeSileroVadModel();

class VadService
{
public:
  VadService();
  explicit VadService(std::unique_ptr<VadModel> model);
  ~VadService();

  VadService(const VadService&) = delete;
  VadService& operator=(const VadService&) = delete;

  std::optional<VadTurn> process(const VadProcessInput& input);

  bool listen(const VadListenInput& input);

  bool inSpeech() const;

  float lastProb() const;

  void reset();

  static bool isLoaded();

private:
  float nextWindow();
  void keepPreRoll(int frames);

  VadConfig cfg_;
  std::unique_ptr<VadModel> model_;
  std::vector<float> context_;
  SampleRing pending_;
  std::vector<float> window_;
  std::vector<float> preRoll_;
  std::vector<float> buffer_;
  bool speech_{false};
  int bargeCounter_{0};
  float bargeProbSum_{0.0F};
  int startCounter_{0};
  int silenceCounter_{0};
  int frameCounter_{0};
  float speechProbSum_{0.0F};
  float lastProb_{0.0F};
};
