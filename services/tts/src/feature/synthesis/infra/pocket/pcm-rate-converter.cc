#include "pcm-rate-converter.hxx"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace
{
constexpr float kInt16Scale = 32767.0F;
constexpr float kInt16Range = 32768.0F;
constexpr std::size_t kFlushSamples = 128;
}

PcmRateConverter::PcmRateConverter(AudioResamplerInput input)
    : resampler_(input), passthrough_(input.sourceRate == input.targetRate)
{
}

std::vector<float> PcmRateConverter::process(std::span<const float> samples)
{
  if (passthrough_)
    return {samples.begin(), samples.end()};
  scratch_.resize(samples.size());
  std::ranges::transform(samples, scratch_.begin(), [](float sample) {
    return static_cast<std::int16_t>(std::lround(std::clamp(sample, -1.0F, 1.0F) * kInt16Scale));
  });
  const auto converted = resampler_.process(scratch_.data(), scratch_.size());
  std::vector<float> out(converted.size());
  std::ranges::transform(converted, out.begin(),
                         [](std::int16_t sample) { return static_cast<float>(sample) / kInt16Range; });
  return out;
}

std::vector<float> PcmRateConverter::flush()
{
  if (passthrough_)
    return {};
  const std::vector<float> silence(kFlushSamples, 0.0F);
  return process(silence);
}
