#pragma once

#include <cstdint>
#if ARGUS_HAS_RNNOISE
#include <rnnoise.h>
#else
struct DenoiseState;
#endif
#include <shared/wrapper/audio/audio-resampler.hxx>
#include <vector>

class NoiseSuppressor
{
public:
  NoiseSuppressor();
  ~NoiseSuppressor();

  NoiseSuppressor(const NoiseSuppressor&) = delete;
  NoiseSuppressor& operator=(const NoiseSuppressor&) = delete;

  // Denoise a chunk of 16 kHz mono float samples in [-1, 1].
  void process(const std::vector<float>& in, std::vector<float>& out);

  // Reset the adaptive model and resamplers (start/end of a session).
  void reset();

  bool available() const { return state_ != nullptr; }

  // Mean RNNoise voice probability of the last processed chunk (0..1).
  float lastVoiceProb() const { return lastVoiceProb_; }

  // True right after a voiced chunk; lets the caller keep the denoiser warm.
  bool recentVoice() const { return lastVoiceProb_ > 0.35F; }

private:
  void applyAgc(const std::vector<float>& in, std::vector<float>& out);

  DenoiseState* state_{nullptr};
  AudioResampler upsampler_{{.sourceRate = 16000, .targetRate = 48000}};
  AudioResampler downsampler_{{.sourceRate = 48000, .targetRate = 16000}};
  std::vector<float> pending48_;
  // Consumed frames stay in pending48_ until a compaction threshold is hit.
  size_t pending48Offset_{0};
  // Reusable scratch buffers: the hot path allocates nothing.
  std::vector<float> workBoosted_;
  std::vector<int16_t> workI16_;
  std::vector<int16_t> workUp48_;
  std::vector<float> workDenoised_;
  std::vector<float> workFrame_;
  std::vector<int16_t> workD48_;
  std::vector<int16_t> workDown16_;
  // AGC state (16 kHz domain).
  float agcRms_{0.0F};
  float agcGain_{1.0F};
  float lastVoiceProb_{0.0F};
};
