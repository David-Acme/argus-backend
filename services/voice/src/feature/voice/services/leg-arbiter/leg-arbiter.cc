#include <feature/voice/services/leg-arbiter/leg-arbiter.hxx>

bool identifiesHolder(VoiceSpeakerVerdict voice, VoiceSpeakerVerdict face)
{
  const bool positive =
      voice == VoiceSpeakerVerdict::Holder || face == VoiceSpeakerVerdict::Holder;
  const bool contradicted =
      voice == VoiceSpeakerVerdict::OtherKnown || face == VoiceSpeakerVerdict::OtherKnown;
  return positive && !contradicted;
}

TakeoverOutcome decideTakeover(const TakeoverInput& input)
{
  if (input.candidate != ConversationLegKind::Camera)
    return TakeoverOutcome::Allowed;
  return identifiesHolder(input.voice, input.face) ? TakeoverOutcome::Allowed
                                                   : TakeoverOutcome::RefusedUnverified;
}

TurnIdentity identifyCameraTurn(const CameraTurnInput& input)
{
  if (identifiesHolder(input.voice, input.face))
    return TurnIdentity::Verified;
  const bool contradicted = input.voice == VoiceSpeakerVerdict::OtherKnown ||
                            input.face == VoiceSpeakerVerdict::OtherKnown;
  if (input.shortTurn && input.inheritanceOpen && !contradicted)
    return TurnIdentity::Verified;
  return TurnIdentity::Unverified;
}

bool inheritanceOpen(const InheritanceInput& input)
{
  const int64_t elapsed = input.nowSeconds - input.verifiedAtSeconds;
  return elapsed >= 0 && elapsed <= input.windowSeconds;
}
