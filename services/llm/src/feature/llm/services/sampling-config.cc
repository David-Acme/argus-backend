#include "sampling-config.hxx"

#include <config/config-service.hxx>

#include <algorithm>

SamplingConfig resolveSampling()
{
  SamplingConfig sampling;
  sampling.maxTokens = std::clamp<int32_t>(ConfigService::getInt("llm.max_tokens"), 16, 4096);
  sampling.temperature = static_cast<float>(std::clamp(ConfigService::getDouble("llm.temperature"), 0.0, 2.0));
  if (const int value = ConfigService::getInt("llm.top_k"); value > 0)
    sampling.topK = value;
  if (const double value = ConfigService::getDouble("llm.top_p"); value > 0.0)
    sampling.topP = static_cast<float>(value);
  if (const int value = ConfigService::getInt("llm.penalty_last_n"); value > 0)
    sampling.penaltyLastN = value;
  if (const double value = ConfigService::getDouble("llm.penalty_repeat"); value > 0.0)
    sampling.penaltyRepeat = static_cast<float>(value);
  sampling.penaltyFreq = static_cast<float>(std::max(0.0, ConfigService::getDouble("llm.penalty_freq")));
  sampling.penaltyPresent = static_cast<float>(std::max(0.0, ConfigService::getDouble("llm.penalty_present")));
  if (const int value = ConfigService::getInt("llm.seed"); value > 0)
    sampling.seed = static_cast<uint32_t>(value);
  return sampling;
}
