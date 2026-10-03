#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

struct SpeechQuality
{
  float speechSeconds{0.0F};
  float snrDb{0.0F};
  float clippedRatio{0.0F};
  size_t speechBegin{0};
  size_t speechEnd{0};
};

enum class SpeechProblem : uint8_t
{
  None = 0,
  TooShort,
  TooNoisy,
  Clipped
};

struct SpeechRequirement
{
  float minSpeechSeconds{0.0F};
  float minSnrDb{0.0F};
};

namespace speech_quality
{
inline constexpr float kMaxClippedRatio = 0.01F;

[[nodiscard]] SpeechQuality measure(std::span<const float> samples);

[[nodiscard]] SpeechProblem judge(const SpeechQuality& quality,
                                  const SpeechRequirement& requirement);

[[nodiscard]] std::span<const float> speechSpan(std::span<const float> samples,
                                                const SpeechQuality& quality);
}
