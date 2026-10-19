#pragma once

#include <feature/watch/conversation-policy.hxx>

#include <llm/conversation-surface.hxx>
#include <voice/voice-lang.hxx>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

struct SeenFaceEvidence
{
  int64_t personId{0};
  int64_t userId{0};
  std::string name;
  std::string role;
  float confidence{0.0F};
  bool accepted{false};
  bool visitor{false};
  bool trusted{false};
};

struct HeardVoiceEvidence
{
  bool matched{false};
  int64_t userId{0};
  std::string name;
  float score{0.0F};
};

struct SpeakerResolutionInput
{
  std::vector<SeenFaceEvidence> faces;
  std::optional<HeardVoiceEvidence> voice;
  VoiceLang lang{VoiceLang::Es};
};

struct SpeakerResolution
{
  ConversationAudience audience{ConversationAudience::Guest};
  ConversationSurface surface{ConversationSurface::CameraVisitor};
  int64_t userId{0};
  std::string role;
  std::string name;
  bool disagreement{false};
  int64_t strangerPersonId{0};
  std::vector<std::string> notes;
};

[[nodiscard]] SpeakerResolution resolveSpeaker(const SpeakerResolutionInput& input);

[[nodiscard]] std::string floorNote(VoiceLang lang);
[[nodiscard]] std::string guestNote(VoiceLang lang);
[[nodiscard]] std::string visitorNote(VoiceLang lang);
[[nodiscard]] std::string discreetNote(VoiceLang lang);
