#pragma once

#include <cstddef>
#include <span>
#include <vector>

struct PauseLimiterConfig
{
  int sampleRate{0};
  int maxPauseMs{0};
  float silenceRms{0.0F};
};

class PauseLimiter
{
public:
  explicit PauseLimiter(PauseLimiterConfig config);

  [[nodiscard]] std::vector<float> process(std::span<const float> pcm);
  [[nodiscard]] std::vector<float> flush();

private:
  [[nodiscard]] bool quiet(std::span<const float> frame) const;
  void push(std::vector<float>& out, std::span<const float> frame, bool isQuiet);

  std::size_t frameSamples_;
  std::size_t maxQuietSamples_;
  float silenceRms_;
  std::vector<float> carry_;
  std::vector<float> held_;
  std::size_t heldQuietSamples_{0};
  std::size_t runQuietSamples_{0};
};
