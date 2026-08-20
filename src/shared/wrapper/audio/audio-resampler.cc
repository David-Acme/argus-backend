#include "audio-resampler.hxx"

#include <algorithm>
#include <cmath>

AudioResampler::AudioResampler(AudioResamplerInput input)
    : sourceRate_(input.sourceRate), targetRate_(input.targetRate),
      ratio_(static_cast<double>(input.sourceRate) /
             static_cast<double>(input.targetRate)),
      cutoff_(0.45 * std::min(input.sourceRate, input.targetRate) /
              static_cast<double>(input.sourceRate))
{
  window_.resize(2 * kSincHalf + 1);
  for (int j = -kSincHalf; j <= kSincHalf; ++j) {
    const double n = static_cast<double>(j + kSincHalf);
    const double N = static_cast<double>(2 * kSincHalf);
    window_[static_cast<size_t>(j + kSincHalf)] =
        0.42 - 0.5 * std::cos(2.0 * M_PI * n / N) +
        0.08 * std::cos(4.0 * M_PI * n / N);
  }
  reset();
}

void AudioResampler::reset()
{
  history_.assign(kSincHalf, 0);
  pos_ = static_cast<double>(kSincHalf);
}

double AudioResampler::tap(double t) const
{
  if (std::fabs(t) < 1e-12)
    return 2.0 * cutoff_;
  return std::sin(2.0 * M_PI * cutoff_ * t) / (M_PI * t);
}

int16_t AudioResampler::sampleAt(double pos)
{
  const size_t i0 = static_cast<size_t>(pos);
  const double fraction = pos - static_cast<double>(i0);
  // Group near-identical fractions (e.g. repeated 1/3 with ulp drift) into
  // one table; the table is the product of tap * window for all j.
  const int64_t key = static_cast<int64_t>(std::llround(fraction * 1e9));
  const std::vector<double>* weights = nullptr;
  auto it = tapCache_.find(key);
  if (it != tapCache_.end()) {
    weights = &it->second;
  } else {
    std::vector<double> computed(2 * kSincHalf + 1);
    for (int j = -kSincHalf; j <= kSincHalf; ++j) {
      computed[static_cast<size_t>(j + kSincHalf)] =
          tap(static_cast<double>(j) - fraction) *
          window_[static_cast<size_t>(j + kSincHalf)];
    }
    auto inserted = tapCache_.emplace(key, std::move(computed));
    weights = &inserted.first->second;
  }

  double acc = 0.0;
  double wsum = 0.0;
  for (int j = -kSincHalf; j <= kSincHalf; ++j) {
    const long idx = static_cast<long>(i0) + j;
    if (idx < 0 || idx >= static_cast<long>(history_.size()))
      continue;
    const double weight = (*weights)[static_cast<size_t>(j + kSincHalf)];
    acc += static_cast<double>(history_[static_cast<size_t>(idx)]) * weight;
    wsum += weight;
  }
  const double value = wsum > 1e-9 ? acc / wsum : 0.0;
  return static_cast<int16_t>(std::clamp(value, -32768.0, 32767.0));
}

void AudioResampler::process(const int16_t* samples, size_t count,
                             std::vector<int16_t>& out)
{
  out.clear();
  if (count == 0)
    return;
  if (sourceRate_ == targetRate_) {
    out.assign(samples, samples + count);
    return;
  }

  history_.insert(history_.end(), samples, samples + count);
  while (pos_ + static_cast<double>(kSincHalf) <
         static_cast<double>(history_.size())) {
    out.push_back(sampleAt(pos_));
    pos_ += ratio_;
  }

  const size_t floorPos = static_cast<size_t>(pos_);
  const size_t keep = floorPos > static_cast<size_t>(kSincHalf)
                          ? floorPos - static_cast<size_t>(kSincHalf)
                          : 0;
  if (keep > 0) {
    history_.erase(history_.begin(), history_.begin() + static_cast<long>(keep));
    pos_ -= static_cast<double>(keep);
  }
}