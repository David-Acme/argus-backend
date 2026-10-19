#pragma once

#include <feature/voice/speaker-verdict.hxx>
#include <llm/conversation-leg.hxx>

#include <cstdint>

enum class TakeoverOutcome : uint8_t
{
  Allowed,
  RefusedUnverified
};

enum class TurnIdentity : uint8_t
{
  Verified,
  Unverified
};

struct TakeoverInput
{
  ConversationLegKind candidate{ConversationLegKind::Phone};
  VoiceSpeakerVerdict voice{VoiceSpeakerVerdict::Unknown};
  VoiceSpeakerVerdict face{VoiceSpeakerVerdict::Unknown};
};

struct CameraTurnInput
{
  VoiceSpeakerVerdict voice{VoiceSpeakerVerdict::Unknown};
  VoiceSpeakerVerdict face{VoiceSpeakerVerdict::Unknown};
  bool shortTurn{false};
  bool inheritanceOpen{false};
};

struct InheritanceInput
{
  int64_t verifiedAtSeconds{0};
  int64_t nowSeconds{0};
  int64_t windowSeconds{0};
};

[[nodiscard]] bool identifiesHolder(VoiceSpeakerVerdict voice, VoiceSpeakerVerdict face);

[[nodiscard]] TakeoverOutcome decideTakeover(const TakeoverInput& input);

[[nodiscard]] TurnIdentity identifyCameraTurn(const CameraTurnInput& input);

[[nodiscard]] bool inheritanceOpen(const InheritanceInput& input);
