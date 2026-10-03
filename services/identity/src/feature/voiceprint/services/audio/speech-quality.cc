#include "speech-quality.hxx"

#include <algorithm>
#include <cmath>
#include <numeric>
#include <vector>

namespace
{

constexpr size_t kFrameSamples = 320;
constexpr float kFrameSeconds = 0.02F;
constexpr size_t kPaddingFrames = 10;
constexpr double kNoisePercentile = 0.05;
constexpr double kPeakPercentile = 0.95;
constexpr double kSpeechMarginDb = 6.0;
constexpr double kDynamicRangeDb = 35.0;
constexpr double kSpeechFloorDb = -55.0;
constexpr double kMaxSnrDb = 60.0;
constexpr double kPowerFloor = 1e-10;
constexpr float kClipLevel = 0.99F;

double decibels(double power)
{
  return 10.0 * std::log10(power + kPowerFloor);
}

std::vector<double> framePowers(std::span<const float> samples)
{
  const size_t frames = samples.size() / kFrameSamples;
  std::vector<double> powers;
  powers.reserve(frames);
  for (size_t frame = 0; frame < frames; ++frame) {
    const auto window = samples.subspan(frame * kFrameSamples, kFrameSamples);
    const double energy =
        std::transform_reduce(window.begin(), window.end(), 0.0, std::plus<>(),
                              [](float value) {
                                return static_cast<double>(value) *
                                       static_cast<double>(value);
                              });
    powers.push_back(energy / static_cast<double>(kFrameSamples));
  }
  return powers;
}

double meanOf(std::span<const double> values)
{
  if (values.empty())
    return 0.0;
  return std::accumulate(values.begin(), values.end(), 0.0) /
         static_cast<double>(values.size());
}

size_t percentileIndex(size_t count, double fraction)
{
  return std::min(count - 1,
                  static_cast<size_t>(static_cast<double>(count) * fraction));
}

}

namespace speech_quality
{

SpeechQuality measure(std::span<const float> samples)
{
  SpeechQuality quality;
  if (samples.empty())
    return quality;

  const auto clipped = std::ranges::count_if(samples, [](float value) {
    return std::fabs(value) >= kClipLevel;
  });
  quality.clippedRatio =
      static_cast<float>(clipped) / static_cast<float>(samples.size());

  const std::vector<double> powers = framePowers(samples);
  if (powers.empty())
    return quality;

  std::vector<double> sorted = powers;
  std::ranges::sort(sorted);
  const size_t count = sorted.size();
  const double noiseDb =
      decibels(sorted[percentileIndex(count, kNoisePercentile)]);
  const double peakDb =
      decibels(sorted[percentileIndex(count, kPeakPercentile)]);
  const double threshold = std::max(
      {noiseDb + kSpeechMarginDb, peakDb - kDynamicRangeDb, kSpeechFloorDb});

  size_t speechFrames = 0;
  size_t first = count;
  size_t last = 0;
  for (size_t frame = 0; frame < count; ++frame) {
    if (decibels(powers[frame]) <= threshold)
      continue;
    ++speechFrames;
    first = std::min(first, frame);
    last = frame;
  }
  if (speechFrames == 0)
    return quality;

  quality.speechSeconds = static_cast<float>(speechFrames) * kFrameSeconds;
  const std::span<const double> ordered = sorted;
  const double loud = meanOf(ordered.subspan(count / 2));
  const double quiet = meanOf(ordered.first(std::max<size_t>(1, count / 10)));
  quality.snrDb = static_cast<float>(
      std::clamp(decibels(loud) - decibels(quiet), 0.0, kMaxSnrDb));
  quality.speechBegin =
      (first > kPaddingFrames ? first - kPaddingFrames : 0) * kFrameSamples;
  quality.speechEnd =
      std::min(samples.size(), (last + 1 + kPaddingFrames) * kFrameSamples);
  return quality;
}

SpeechProblem judge(const SpeechQuality& quality,
                    const SpeechRequirement& requirement)
{
  if (quality.clippedRatio > kMaxClippedRatio)
    return SpeechProblem::Clipped;
  if (quality.speechSeconds < requirement.minSpeechSeconds)
    return SpeechProblem::TooShort;
  if (quality.snrDb < requirement.minSnrDb)
    return SpeechProblem::TooNoisy;
  return SpeechProblem::None;
}

std::span<const float> speechSpan(std::span<const float> samples,
                                  const SpeechQuality& quality)
{
  if (quality.speechEnd <= quality.speechBegin ||
      quality.speechEnd > samples.size())
    return samples;
  return samples.subspan(quality.speechBegin,
                         quality.speechEnd - quality.speechBegin);
}

}
