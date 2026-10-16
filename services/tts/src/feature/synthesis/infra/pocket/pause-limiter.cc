#include "pause-limiter.hxx"

#include <algorithm>
#include <cmath>

namespace
{
constexpr int kFrameMs = 10;
}

PauseLimiter::PauseLimiter(PauseLimiterConfig config)
    : frameSamples_(static_cast<std::size_t>(config.sampleRate) * kFrameMs / 1000),
      maxQuietSamples_(static_cast<std::size_t>(config.sampleRate) * static_cast<std::size_t>(config.maxPauseMs) / 1000),
      silenceRms_(config.silenceRms)
{
}

bool PauseLimiter::quiet(std::span<const float> frame) const
{
  double energy = 0.0;
  for (const float sample : frame)
    energy += static_cast<double>(sample) * static_cast<double>(sample);
  return std::sqrt(energy / static_cast<double>(frame.size())) < silenceRms_;
}

void PauseLimiter::push(std::vector<float>& out, std::span<const float> frame, bool isQuiet)
{
  if (!isQuiet) {
    out.insert(out.end(), held_.begin(), held_.end());
    held_.clear();
    heldQuietSamples_ = 0;
    out.insert(out.end(), frame.begin(), frame.end());
    runQuietSamples_ = 0;
    return;
  }
  runQuietSamples_ += frame.size();
  if (runQuietSamples_ <= maxQuietSamples_) {
    held_.insert(held_.end(), frame.begin(), frame.end());
    heldQuietSamples_ += frame.size();
  }
}

std::vector<float> PauseLimiter::process(std::span<const float> pcm)
{
  std::vector<float> out;
  carry_.insert(carry_.end(), pcm.begin(), pcm.end());
  std::size_t offset = 0;
  while (carry_.size() - offset >= frameSamples_) {
    const std::span<const float> frame(carry_.data() + offset, frameSamples_);
    push(out, frame, quiet(frame));
    offset += frameSamples_;
  }
  carry_.erase(carry_.begin(), carry_.begin() + static_cast<std::ptrdiff_t>(offset));
  return out;
}

std::vector<float> PauseLimiter::flush()
{
  std::vector<float> out;
  if (!carry_.empty()) {
    const std::span<const float> tail(carry_);
    push(out, tail, quiet(tail));
    carry_.clear();
  }
  out.insert(out.end(), held_.begin(), held_.end());
  held_.clear();
  heldQuietSamples_ = 0;
  runQuietSamples_ = 0;
  return out;
}
