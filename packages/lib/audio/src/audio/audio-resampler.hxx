#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

struct AudioResamplerInput
{
  int sourceRate;
  int targetRate;
};

struct ResamplerTap
{
  size_t whole{0};
  int64_t phase{0};
};

class AudioResampler
{
public:
  explicit AudioResampler(AudioResamplerInput input);

  std::vector<int16_t> process(const int16_t* samples, size_t count);
  void processInto(std::span<const int16_t> samples, std::vector<int16_t>& out);
  void reset();

  [[nodiscard]] int sourceRate() const { return sourceRate_; }
  [[nodiscard]] int targetRate() const { return targetRate_; }

private:
  static constexpr int kSincHalf = 32;
  static constexpr int kTaps = 2 * kSincHalf + 1;
  static constexpr int64_t kMaxPhases = 1024;

  [[nodiscard]] int16_t sampleAt(const ResamplerTap& tap) const;

  int sourceRate_;
  int targetRate_;
  int64_t stepWhole_{0};
  int64_t stepFraction_{0};
  int64_t denominator_{1};
  int64_t phases_{1};
  size_t posWhole_{0};
  int64_t posFraction_{0};
  std::vector<int16_t> history_;
  std::vector<double> taps_;
};
