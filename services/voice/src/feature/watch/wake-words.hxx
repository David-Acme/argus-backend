#pragma once

#include <voice/voice-lang.hxx>

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

enum class WakePosition : uint8_t
{
  None,
  Vocative,
  Referential
};

struct WakeWord
{
  std::string folded;
  bool afterComma{false};
};

struct WakeMatch
{
  WakePosition position{WakePosition::None};
  int distance{-1};
  size_t index{0};
  std::string request;
};

struct WakeRuleInput
{
  std::string_view text;
  int maxDistance{2};
};

[[nodiscard]] std::string foldText(std::string_view text);

[[nodiscard]] std::vector<WakeWord> wakeWordsOf(std::string_view text);

[[nodiscard]] int editDistance(std::string_view left, std::string_view right);

[[nodiscard]] int wakeDistance(std::string_view folded);

[[nodiscard]] bool isWakeWord(std::string_view folded, int maxDistance);

[[nodiscard]] bool mentionsWake(const WakeRuleInput& input);

[[nodiscard]] WakeMatch matchWake(const WakeRuleInput& input);

[[nodiscard]] float wakeConfidence(const WakeMatch& match);

[[nodiscard]] std::string withoutSelfName(std::string_view text, VoiceLang lang);

[[nodiscard]] bool closingPhrase(std::string_view text);

[[nodiscard]] bool handoffRequest(std::string_view text);

[[nodiscard]] bool trailsOff(std::string_view text);

[[nodiscard]] bool panicPhrase(std::string_view text);
