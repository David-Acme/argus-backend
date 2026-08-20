#pragma once

#include <cstdint>
#include <unordered_map>
#include <vector>

struct AudioResamplerInput
{
  int sourceRate;
  int targetRate;
};

class AudioResampler
{
public:
  explicit AudioResampler(AudioResamplerInput input);

  void process(const int16_t* samples, size_t count, std::vector<int16_t>& out);
  void reset();

  int sourceRate() const { return sourceRate_; }
  int targetRate() const { return targetRate_; }

private:
  static constexpr int kSincHalf = 32;

  double tap(double t) const;
  int16_t sampleAt(double pos);

  int sourceRate_;
  int targetRate_;
  double ratio_;
  double cutoff_;
  double pos_{0.0};
  std::vector<int16_t> history_;
  std::vector<double> window_;

  // Sinc-tap tables keyed by the (rounded) fractional position. A resampler
  // visits only a handful of fractions (e.g. 16k->48k: 0, 1/3, 2/3), so the
  // 65 sin/cos per output sample collapse to ~3 tables computed once.
  mutable std::unordered_map<int64_t, std::vector<double>> tapCache_;
};