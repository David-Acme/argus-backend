#pragma once

#include <cstdint>
#include <span>
#include <vector>

namespace turn
{

enum class GlinerOverlap : unsigned char
{
  Flat,
  Longest,
  Allow
};

struct GlinerCandidate
{
  std::int32_t query{0};
  std::int32_t start{0};
  std::int32_t end{0};
  float logit{0.0F};
};

struct GlinerSpan
{
  std::int32_t query{0};
  std::int32_t start{0};
  std::int32_t end{0};
  float probability{0.0F};
};

struct GlinerDecodeInput
{
  std::span<const GlinerCandidate> candidates;
  std::span<const float> queryThresholds;
  float defaultThreshold{0.5F};
  int maxWidth{8};
  GlinerOverlap overlap{GlinerOverlap::Flat};
};

struct GlinerOffsets
{
  std::int32_t begin{0};
  std::int32_t end{0};
};

[[nodiscard]] float glinerSigmoid(float value);

[[nodiscard]] std::vector<GlinerSpan> glinerDecode(const GlinerDecodeInput& input);

[[nodiscard]] GlinerOffsets glinerCharacterOffsets(const GlinerSpan& span,
                                                   std::span<const std::int32_t> startMappings,
                                                   std::span<const std::int32_t> endMappings);

}
