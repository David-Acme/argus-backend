#pragma once

#include <cstdint>
#include <string>

enum class PhraseKind : uint8_t
{
  Trigger = 0,
  Confirmation,
  StatementStart,
  RecallMarker,
  Interrogative,
  Filler,
  Cancellation
};

inline std::string phraseKindToString(PhraseKind k)
{
  switch (k) {
    case PhraseKind::Confirmation:
      return "confirmation";
    case PhraseKind::StatementStart:
      return "statement_start";
    case PhraseKind::RecallMarker:
      return "recall_marker";
    case PhraseKind::Interrogative:
      return "interrogative";
    case PhraseKind::Filler:
      return "filler";
    case PhraseKind::Cancellation:
      return "cancellation";
    default:
      return "trigger";
  }
}

inline PhraseKind phraseKindFromString(const std::string& s)
{
  if (s == "confirmation")
    return PhraseKind::Confirmation;
  if (s == "statement_start")
    return PhraseKind::StatementStart;
  if (s == "recall_marker")
    return PhraseKind::RecallMarker;
  if (s == "interrogative")
    return PhraseKind::Interrogative;
  if (s == "filler")
    return PhraseKind::Filler;
  if (s == "cancellation")
    return PhraseKind::Cancellation;
  return PhraseKind::Trigger;
}
