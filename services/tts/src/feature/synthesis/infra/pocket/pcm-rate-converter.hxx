#pragma once

#include <audio/audio-resampler.hxx>

#include <cstdint>
#include <span>
#include <vector>

class PcmRateConverter
{
public:
  explicit PcmRateConverter(AudioResamplerInput input);

  [[nodiscard]] std::vector<float> process(std::span<const float> samples);
  [[nodiscard]] std::vector<float> flush();
  [[nodiscard]] bool passthrough() const { return passthrough_; }

private:
  AudioResampler resampler_;
  bool passthrough_{false};
  std::vector<std::int16_t> scratch_;
};
