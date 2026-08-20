#pragma once

#include <cstdint>
#include <string>

// What the assistant is reacting WITH. The backend owns the meaning; the
// client owns how it looks (see REACTION_EXPRESSION in the frontend), so
// renaming an avatar expression never touches C++.
enum class ReactionKind : uint8_t
{
  Idle = 0,
  Warm,
  Thinking,
  Uncertain,
  Attentive,
  Recognizing,
  Curious,
  Acknowledging,
  Confused,
  Alarmed
};

inline std::string reactionKindToString(ReactionKind kind)
{
  switch (kind) {
    case ReactionKind::Warm:
      return "warm";
    case ReactionKind::Thinking:
      return "thinking";
    case ReactionKind::Uncertain:
      return "uncertain";
    case ReactionKind::Attentive:
      return "attentive";
    case ReactionKind::Recognizing:
      return "recognizing";
    case ReactionKind::Curious:
      return "curious";
    case ReactionKind::Acknowledging:
      return "acknowledging";
    case ReactionKind::Confused:
      return "confused";
    case ReactionKind::Alarmed:
      return "alarmed";
    default:
      return "idle";
  }
}

inline ReactionKind reactionKindFromString(const std::string& name)
{
  if (name == "warm")
    return ReactionKind::Warm;
  if (name == "thinking")
    return ReactionKind::Thinking;
  if (name == "uncertain")
    return ReactionKind::Uncertain;
  if (name == "attentive")
    return ReactionKind::Attentive;
  if (name == "recognizing")
    return ReactionKind::Recognizing;
  if (name == "curious")
    return ReactionKind::Curious;
  if (name == "acknowledging")
    return ReactionKind::Acknowledging;
  if (name == "confused")
    return ReactionKind::Confused;
  if (name == "alarmed")
    return ReactionKind::Alarmed;
  return ReactionKind::Idle;
}

// Every field is optional in practice: labs/voice-test fills all of them,
// VoiceSessionService fills what it has until MemoryService reaches it.
// recallHits < 0 means recall was never consulted, which is NOT the same as
// consulted and empty.
struct ReactionSignals
{
  std::string text;
  std::string lang;
  bool captureStored = false;
  bool captureQueued = false;
  int recallHits = -1;
  bool cameraIntent = false;
  bool sttFailed = false;
  bool systemAlert = false;
};

struct Reaction
{
  ReactionKind kind = ReactionKind::Idle;
  float intensity = 0.0F;
  std::string because;
};
