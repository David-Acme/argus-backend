#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

struct AudioLevelReport
{
  size_t frames{0};
  size_t samples{0};
  double rmsDbfs{-120.0};
  int peak{0};
};

class AudioLevelMeter
{
public:
  explicit AudioLevelMeter(int64_t intervalMs) : intervalMs_(intervalMs) {}

  std::optional<AudioLevelReport> add(std::span<const int16_t> samples, int64_t nowMs)
  {
    if (!started_) {
      started_ = true;
      windowStartMs_ = nowMs;
    }
    ++frames_;
    for (const int16_t sample : samples) {
      const int value = sample < 0 ? -static_cast<int>(sample) : static_cast<int>(sample);
      sumSquares_ += static_cast<double>(sample) * static_cast<double>(sample);
      if (value > peak_)
        peak_ = value;
    }
    samples_ += samples.size();
    if (nowMs - windowStartMs_ < intervalMs_)
      return std::nullopt;
    return flush(nowMs);
  }

private:
  std::optional<AudioLevelReport> flush(int64_t nowMs)
  {
    AudioLevelReport report;
    report.frames = frames_;
    report.samples = samples_;
    report.peak = peak_;
    if (samples_ > 0 && sumSquares_ > 0.0) {
      const double rms = std::sqrt(sumSquares_ / static_cast<double>(samples_)) / 32768.0;
      report.rmsDbfs = std::max(-120.0, 20.0 * std::log10(rms));
    }
    frames_ = 0;
    samples_ = 0;
    sumSquares_ = 0.0;
    peak_ = 0;
    windowStartMs_ = nowMs;
    return report;
  }

  int64_t intervalMs_;
  bool started_{false};
  int64_t windowStartMs_{0};
  size_t frames_{0};
  size_t samples_{0};
  double sumSquares_{0.0};
  int peak_{0};
};
