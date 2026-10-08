#include "playout-gain.hxx"

#include <algorithm>
#include <cmath>
#include <limits>

namespace
{

constexpr float kUnity = 1.0F;

struct Ramp
{
  float from{0.0F};
  float to{0.0F};
};

int16_t scaled(int16_t sample, float gain)
{
  const float value = std::round(static_cast<float>(sample) * gain);
  return static_cast<int16_t>(std::clamp(value, static_cast<float>(std::numeric_limits<int16_t>::min()),
                                         static_cast<float>(std::numeric_limits<int16_t>::max())));
}

void ramp(std::span<int16_t> samples, Ramp gains)
{
  if (samples.empty())
    return;
  if (samples.size() == 1) {
    samples.front() = scaled(samples.front(), gains.to);
    return;
  }
  const auto last = static_cast<float>(samples.size() - 1);
  for (size_t index = 0; index < samples.size(); ++index) {
    const float position = static_cast<float>(index) / last;
    samples[index] = scaled(samples[index], gains.from + (gains.to - gains.from) * position);
  }
}

float stepOf(float span, int frames)
{
  return frames > 0 ? span / static_cast<float>(frames) : span;
}

}

PlayoutGain::PlayoutGain(PlayoutGainSteps steps) : steps_(steps)
{
}

void PlayoutGain::duck(bool ducked)
{
  target_ = ducked ? steps_.duckGain : kUnity;
}

void PlayoutGain::apply(std::span<int16_t> frame)
{
  const float span = kUnity - steps_.duckGain;
  const float from = current_;
  if (current_ > target_)
    current_ = std::max(target_, current_ - stepOf(span, steps_.duckFrames));
  else if (current_ < target_)
    current_ = std::min(target_, current_ + stepOf(span, steps_.releaseFrames));
  if (from == kUnity && current_ == kUnity)
    return;
  ramp(frame, {.from = from, .to = current_});
}

void PlayoutGain::fadeOut(std::span<int16_t> tail)
{
  ramp(tail, {.from = current_, .to = 0.0F});
  current_ = kUnity;
  target_ = kUnity;
}
