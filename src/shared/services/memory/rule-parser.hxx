#pragma once

#include <optional>
#include <shared/enums.hxx>
#include <string>

struct RuleParseInput
{
  std::string text;
  std::string lang;
};

struct RuleParseResult
{
  MemoryType type;
  std::string content;
  int priority;
};

class RuleParser
{
public:
  RuleParser() = delete;
  ~RuleParser() = delete;

  static std::optional<RuleParseResult> parse(const RuleParseInput& input);
};
