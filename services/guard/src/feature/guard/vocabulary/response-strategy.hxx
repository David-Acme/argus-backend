#pragma once

#include <cstdint>
#include <string>

enum class ResponseStrategy : uint8_t
{
  Ordered = 0,
  InsideFirst,
  Everyone,
  NightQuiet
};

inline std::string responseStrategyToString(ResponseStrategy strategy)
{
  switch (strategy) {
    case ResponseStrategy::Ordered:
      return "ordered";
    case ResponseStrategy::InsideFirst:
      return "inside_first";
    case ResponseStrategy::Everyone:
      return "everyone";
    case ResponseStrategy::NightQuiet:
      return "night_quiet";
  }
  return "ordered";
}

enum class ResponseTrigger : uint8_t
{
  Intrusion = 0,
  Escalation,
  Panic,
  Duress,
  Tamper
};
