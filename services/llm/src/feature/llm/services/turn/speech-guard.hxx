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
  ClaimedWithoutTool
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

[[nodiscard]] GuardVerdict check(const GuardInput& input);

[[nodiscard]] std::string_view feedback(GuardVerdict verdict, std::string_view lang);

[[nodiscard]] std::string_view verdictName(GuardVerdict verdict);

[[nodiscard]] std::string_view firstSentenceOf(std::string_view text);

}
