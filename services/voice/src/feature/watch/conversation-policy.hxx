#pragma once

#include <voice/voice-lang.hxx>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

enum class ConversationState : uint8_t
{
  Waking,
  Listening,
  Thinking,
  Speaking,
  Resumable,
  Ended
};

enum class ConversationHandoff : uint8_t
{
  Camera,
  Offering,
  Bridging,
  Device
};

enum class ConversationAudience : uint8_t
{
  Member,
  Discreet,
  Guest,
  Visitor
};

enum class ConversationEnd : uint8_t
{
  None,
  Silence,
  Closing,
  Limit,
  CameraLost,
  BusyLine,
  Handoff,
  Superseded,
  Shutdown,
  Gate,
  Failed,
  Expired
};

[[nodiscard]] std::string_view conversationStateToString(ConversationState state);
[[nodiscard]] std::string_view conversationHandoffToString(ConversationHandoff handoff);
[[nodiscard]] std::string_view conversationAudienceToString(ConversationAudience audience);
[[nodiscard]] std::string_view conversationEndToString(ConversationEnd end);

struct ConversationTurn
{
  int64_t id{0};
  std::string role;
  std::string text;
  int64_t at{0};
  bool final{true};
};

struct ConversationSnapshot
{
  std::string conversationId;
  int64_t userId{0};
  int64_t cameraId{0};
  std::string cameraName;
  ConversationState state{ConversationState::Waking};
  ConversationHandoff handoff{ConversationHandoff::Camera};
  ConversationAudience audience{ConversationAudience::Guest};
  int64_t startedAt{0};
  int64_t updatedAt{0};
  int64_t resumableUntil{0};
  ConversationEnd endReason{ConversationEnd::None};
  std::vector<ConversationTurn> turns;
};

struct ConversationClockInput
{
  int64_t now{0};
  int64_t lastSpeech{0};
  int64_t startedAt{0};
  int64_t idleCloseMs{20000};
  int64_t maxLengthMs{600000};
  bool busy{false};
};

[[nodiscard]] ConversationEnd conversationDue(const ConversationClockInput& input);

struct ReplyTimerInput
{
  int64_t now{0};
  int64_t userTurnAt{0};
  int64_t firstAudioAt{0};
  bool acked{false};
  int64_t ackAfterMs{1500};
  int64_t failAfterMs{8000};
};

enum class ReplyNudge : uint8_t
{
  None,
  Acknowledge,
  GiveUp
};

[[nodiscard]] ReplyNudge replyNudge(const ReplyTimerInput& input);

[[nodiscard]] std::string_view acknowledgementLine(VoiceLang lang);
[[nodiscard]] std::string_view giveUpLine(VoiceLang lang);
[[nodiscard]] std::string cameraNote(VoiceLang lang, std::string_view cameraName);
[[nodiscard]] std::string busyNotice(VoiceLang lang, std::string_view cameraName);
[[nodiscard]] std::string_view handoffLine(VoiceLang lang);
[[nodiscard]] std::string_view continueLine(VoiceLang lang);
[[nodiscard]] std::string mintConversationId();
[[nodiscard]] int64_t epochSeconds();
