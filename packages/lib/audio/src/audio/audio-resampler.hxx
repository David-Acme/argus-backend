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

  std::vector<int16_t> process(const int16_t* samples, size_t count);
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

  // Sinc-tap tables keyed by the rounded fractional position, computed once.
  mutable std::unordered_map<int64_t, std::vector<double>> tapCache_;
};
