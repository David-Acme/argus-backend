#pragma once

#include <cstdint>
#if ARGUS_HAS_RNNOISE
#include <rnnoise.h>
#else
struct DenoiseState;
#endif
#include <audio/audio-resampler.hxx>
#include <vector>

class NoiseSuppressor
{
public:
  NoiseSuppressor();
  ~NoiseSuppressor();

  NoiseSuppressor(const NoiseSuppressor&) = delete;
  NoiseSuppressor& operator=(const NoiseSuppressor&) = delete;

  void process(const std::vector<float>& in, std::vector<float>& out);

  void reset();

  bool available() const { return state_ != nullptr; }

  float lastVoiceProb() const { return lastVoiceProb_; }

  bool recentVoice() const { return lastVoiceProb_ > 0.35F; }

private:
  void applyAgc(const std::vector<float>& in, std::vector<float>& out);

  DenoiseState* state_{nullptr};
  AudioResampler upsampler_{{.sourceRate = 16000, .targetRate = 48000}};
  AudioResampler downsampler_{{.sourceRate = 48000, .targetRate = 16000}};
  std::vector<float> pending48_;
  size_t pending48Offset_{0};
  std::vector<float> workBoosted_;
  std::vector<int16_t> workI16_;
  std::vector<int16_t> workUp48_;
  std::vector<float> workDenoised_;
  std::vector<float> workFrame_;
  std::vector<int16_t> workD48_;
  std::vector<int16_t> workDown16_;
  float agcRms_{0.0F};
  float agcGain_{1.0F};
  float lastVoiceProb_{0.0F};
};
