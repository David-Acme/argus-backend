#pragma once

#include <cstdint>
#include <limits>

struct SamplingConfig
{
  int32_t maxTokens{96};
  float temperature{0.3F};
  int32_t topK{20};
  float topP{0.8F};
  int32_t penaltyLastN{64};
  float penaltyRepeat{1.1F};
  float penaltyFreq{0.0F};
  float penaltyPresent{0.0F};
  uint32_t seed{std::numeric_limits<uint32_t>::max()};
};

[[nodiscard]] SamplingConfig resolveSampling();
