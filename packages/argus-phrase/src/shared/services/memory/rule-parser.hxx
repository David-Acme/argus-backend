#pragma once

#include <cstdint>
#include <optional>
#include <shared/enums.hxx>
#include <string>

class PhraseCatalog;

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
  explicit RuleParser(const PhraseCatalog& catalog) : catalog_(catalog) {}

  std::optional<RuleParseResult> parse(const RuleParseInput& input) const;
  std::optional<RuleParseResult>
  parseStatement(const RuleParseInput& input) const;

  bool isQuestion(const RuleParseInput& input) const;

  // Cancellation: the turn retracts itself, so nothing in it reaches memory formation.
  bool isCancellation(const RuleParseInput& input) const;

  // Carries no fact: empty, a bare tag or a trigger with nothing after it.
  bool isVacuous(const RuleParseInput& input) const;

  std::string stripFillers(const RuleParseInput& input) const;

  bool isFiller(const std::string& phrase, const std::string& lang) const;

private:
  struct ContentBeforeTriggerInput
  {
    const std::string& text;
    const std::string& lowered;
    uint32_t begin;
    uint32_t end;
  };

  std::optional<std::string>
  contentBeforeTrigger(const ContentBeforeTriggerInput& input) const;

  std::string stripTrailingConfirmation(std::string text,
                                        const std::string& lang) const;
  bool isRecallTalk(const std::string& lowered, const std::string& lang) const;

  const PhraseCatalog& catalog_;
};
