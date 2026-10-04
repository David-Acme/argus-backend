#include "sampling-config.hxx"

#include <config/config-service.hxx>

#include <algorithm>
#include <string>

namespace
{
struct DecimalKey
{
  const char* key;
  double fallback;
  double min;
  double max;
};

struct IntegerKey
{
  const char* key;
  int fallback;
  int min;
  int max;
};

float decimal(const DecimalKey& spec)
{
  if (!ConfigService::hasKey(spec.key))
    return static_cast<float>(spec.fallback);
  return static_cast<float>(std::clamp(ConfigService::getDouble(spec.key), spec.min, spec.max));
}

int32_t integer(const IntegerKey& spec)
{
  if (!ConfigService::hasKey(spec.key))
    return spec.fallback;
  return std::clamp(ConfigService::getInt(spec.key), spec.min, spec.max);
}
}

LlmSampling resolveSampling()
{
  const LlmSampling defaults;
  const int32_t seed = integer({.key = "llm.seed", .fallback = 0, .min = 0, .max = 2147483647});
  return {.maxTokens = integer({.key = "llm.max_tokens", .fallback = defaults.maxTokens, .min = 16, .max = 4096}),
          .temperature = decimal({.key = "llm.temperature", .fallback = defaults.temperature, .min = 0.0, .max = 2.0}),
          .topK = integer({.key = "llm.top_k", .fallback = defaults.topK, .min = 1, .max = 200}),
          .topP = decimal({.key = "llm.top_p", .fallback = defaults.topP, .min = 0.05, .max = 1.0}),
          .minP = decimal({.key = "llm.min_p", .fallback = defaults.minP, .min = 0.0, .max = 0.5}),
          .penaltyLastN =
              integer({.key = "llm.penalty_last_n", .fallback = defaults.penaltyLastN, .min = 1, .max = 1024}),
          .penaltyRepeat =
              decimal({.key = "llm.penalty_repeat", .fallback = defaults.penaltyRepeat, .min = 1.0, .max = 2.0}),
          .penaltyFreq = decimal({.key = "llm.penalty_freq", .fallback = defaults.penaltyFreq, .min = 0.0, .max = 2.0}),
          .penaltyPresent =
              decimal({.key = "llm.penalty_present", .fallback = defaults.penaltyPresent, .min = 0.0, .max = 2.0}),
          .seed = seed > 0 ? static_cast<uint32_t>(seed) : defaults.seed};
}
