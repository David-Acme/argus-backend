#include "audio-resampler.hxx"

#include <algorithm>
#include <cmath>
#include <numbers>
#include <numeric>

namespace
{

double sinc(double cutoff, double t)
{
  if (std::fabs(t) < 1e-12)
    return 2.0 * cutoff;
  return std::sin(2.0 * std::numbers::pi * cutoff * t) / (std::numbers::pi * t);
}

struct BlackmanInput
{
  int index{0};
  int taps{0};
};

double blackman(const BlackmanInput& input)
{
  const auto n = static_cast<double>(input.index);
  const auto span = static_cast<double>(input.taps - 1);
  return 0.42 - 0.5 * std::cos(2.0 * std::numbers::pi * n / span) +
         0.08 * std::cos(4.0 * std::numbers::pi * n / span);
}

}

AudioResampler::AudioResampler(AudioResamplerInput input)
    : sourceRate_(input.sourceRate), targetRate_(input.targetRate)
{
  const int64_t divisor = std::gcd(sourceRate_, targetRate_);
  const int64_t step = divisor > 0 ? sourceRate_ / divisor : 1;
  denominator_ = divisor > 0 ? targetRate_ / divisor : 1;
  stepWhole_ = step / denominator_;
  stepFraction_ = step % denominator_;
  phases_ = std::min(denominator_, kMaxPhases);

  const double cutoff =
      sourceRate_ > 0
          ? 0.45 * std::min(sourceRate_, targetRate_) / sourceRate_
          : 0.45;
  taps_.resize(static_cast<size_t>(phases_ * kTaps));
  for (int64_t phase = 0; phase < phases_; ++phase) {
    const double fraction =
        static_cast<double>(phase) / static_cast<double>(phases_);
    for (int j = 0; j < kTaps; ++j)
      taps_[static_cast<size_t>(phase * kTaps + j)] =
          sinc(cutoff, static_cast<double>(j - kSincHalf) - fraction) *
          blackman({.index = j, .taps = kTaps});
  }
  reset();
}

void AudioResampler::reset()
{
  history_.assign(kSincHalf, 0);
  posWhole_ = kSincHalf;
  posFraction_ = 0;
}

int16_t AudioResampler::sampleAt(const ResamplerTap& tap) const
{
  const size_t whole = tap.whole;
  const double* weights = taps_.data() + tap.phase * kTaps;
  double acc = 0.0;
  double wsum = 0.0;
  for (int j = 0; j < kTaps; ++j) {
    const auto idx = static_cast<int64_t>(whole) + j - kSincHalf;
    if (idx < 0 || idx >= static_cast<int64_t>(history_.size()))
      continue;
    acc += static_cast<double>(history_[static_cast<size_t>(idx)]) * weights[j];
    wsum += weights[j];
  }
  const double value = wsum > 1e-9 ? acc / wsum : 0.0;
  return static_cast<int16_t>(std::clamp(value, -32768.0, 32767.0));
}

std::vector<int16_t> AudioResampler::process(const int16_t* samples,
                                             size_t count)
{
  std::vector<int16_t> out;
  processInto({samples, count}, out);
  return out;
}

void AudioResampler::processInto(std::span<const int16_t> samples, std::vector<int16_t>& out)
{
  out.clear();
  if (samples.empty())
    return;
  if (sourceRate_ == targetRate_) {
    out.assign(samples.begin(), samples.end());
    return;
  }

  history_.insert(history_.end(), samples.begin(), samples.end());
  out.reserve(static_cast<size_t>(
      static_cast<double>(samples.size()) * targetRate_ / sourceRate_ + 2.0));
  while (posWhole_ + kSincHalf < history_.size()) {
    out.push_back(sampleAt(
        {.whole = posWhole_, .phase = posFraction_ * phases_ / denominator_}));
    posWhole_ += static_cast<size_t>(stepWhole_);
    posFraction_ += stepFraction_;
    if (posFraction_ >= denominator_) {
      posFraction_ -= denominator_;
      ++posWhole_;
    }
  }

  const size_t keep = posWhole_ > static_cast<size_t>(kSincHalf)
                          ? posWhole_ - static_cast<size_t>(kSincHalf)
                          : 0;
  if (keep > 0) {
    history_.erase(history_.begin(),
                   history_.begin() + static_cast<std::ptrdiff_t>(keep));
    posWhole_ -= keep;
  }
}
