#pragma once

#include "speech-acts.hxx"

#include <string>
#include <string_view>

namespace turn::speech
{

enum class GuardVerdict : unsigned char
{
  Pass,
  NotAQuestion,
  SlotNotAsked,
  SlotReasked,
  OptionsIncomplete,
  ActionUnnamed,
  ArgumentMissing,
  NotYesOrNo,
  ClaimedWithoutTool,
  IdentifierLeaked,
  Impersonated
};

struct GuardInput
{
  const Speech& speech;
  std::string_view reply;
  bool wrote{false};
  bool opened{false};
  bool asked{false};
  bool callsConfirmed{false};
  bool sentenceOnly{false};
};

struct ReleaseWindow
{
  std::string_view text;
  bool settled{false};
};

[[nodiscard]] GuardVerdict check(const GuardInput& input);

[[nodiscard]] std::string_view feedback(GuardVerdict verdict, std::string_view lang);

[[nodiscard]] std::string_view verdictName(GuardVerdict verdict);

[[nodiscard]] ReleaseWindow releaseWindowOf(std::string_view text);

[[nodiscard]] bool hardFailure(GuardVerdict verdict, const Speech& speech);

[[nodiscard]] bool impersonates(std::string_view reply, std::string_view lang);

[[nodiscard]] bool selfIdentifies(std::string_view reply, std::string_view lang);

struct ScrubInput
{
  std::string reply;
  std::string_view lang;
  std::string_view grounding;
};

[[nodiscard]] std::string withoutUngroundedClaims(const ScrubInput& input);

struct SentenceProbe
{
  std::string_view sentence;
  std::string_view lang;
  std::string_view grounding;
};

[[nodiscard]] bool ungroundedSentence(const SentenceProbe& probe);

[[nodiscard]] std::string claimReplacement(std::string_view lang);

}
