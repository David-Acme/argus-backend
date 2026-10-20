#pragma once

#include <feature/watch/conversation-policy.hxx>

#include <voice/voice-lang.hxx>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

enum class ConsentAnswer : uint8_t
{
  None,
  Yes,
  No
};

struct ConsentAnswerInput
{
  std::string_view text;
  int maxWords{6};
  int wakeMaxDistance{2};
};

[[nodiscard]] ConsentAnswer consentAnswer(const ConsentAnswerInput& input);

[[nodiscard]] bool minorStatement(std::string_view text);

struct ConsentTurnInput
{
  VisitorConsent consent{VisitorConsent::Asked};
  std::string_view text;
  int asked{1};
  int maxAsks{2};
  int maxWords{6};
  int wakeMaxDistance{2};
  bool beforeQuestion{false};
  VoiceLang lang{VoiceLang::Es};
};

struct ConsentTurn
{
  VisitorConsent consent{VisitorConsent::Asked};
  bool toAssistant{false};
  bool askedAgain{false};
  bool minorStated{false};
  std::string line;
};

[[nodiscard]] ConsentTurn consentTurn(const ConsentTurnInput& input);

[[nodiscard]] std::string_view consentQuestion(VoiceLang lang);
[[nodiscard]] std::string_view consentAskAgain(VoiceLang lang);
[[nodiscard]] std::string_view consentThanks(VoiceLang lang);
[[nodiscard]] std::string_view consentDeclined(VoiceLang lang);
[[nodiscard]] std::string_view safetyOnlyLine(VoiceLang lang);
[[nodiscard]] std::string_view consentGrantedNote(VoiceLang lang);
