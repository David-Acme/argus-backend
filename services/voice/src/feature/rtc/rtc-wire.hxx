#pragma once

#include <argus/voice/v1/voice.pb.h>

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace rtc_wire
{

inline constexpr std::string_view kAgentIdentity = "argus-voice";
inline constexpr std::string_view kAgentTrackName = "argus-voice";
inline constexpr std::string_view kAgentStateAttribute = "lk.agent.state";

inline constexpr std::string_view kTopicStt = "argus.stt";
inline constexpr std::string_view kTopicAssistant = "argus.assistant";
inline constexpr std::string_view kTopicTurn = "argus.turn";
inline constexpr std::string_view kTopicInterrupted = "argus.interrupted";
inline constexpr std::string_view kTopicAction = "argus.action";
inline constexpr std::string_view kTopicEvent = "argus.event";
inline constexpr std::string_view kTopicDone = "argus.done";

inline constexpr std::string_view kTopicContext = "argus.context";
inline constexpr std::string_view kTopicActionResult = "argus.action_result";
inline constexpr std::string_view kTopicMute = "argus.mute";
inline constexpr std::string_view kTopicSkip = "argus.skip";
inline constexpr std::string_view kTopicHangup = "argus.hangup";

inline constexpr std::size_t kMaxClientPayloadBytes = 4096;

enum class AgentState : uint8_t
{
  Initializing,
  Listening,
  Thinking,
  Speaking
};

enum class DoneReason : uint8_t
{
  Hangup,
  Timeout,
  Revoked,
  Error
};

enum class CallOutcome : uint8_t
{
  NotReported,
  Completed,
  Declined,
  Failed
};

struct CallEnd
{
  std::string_view callId;
  DoneReason reason{DoneReason::Hangup};
  bool userJoined{false};
  bool openingSpoken{false};
};

enum class ClientMessageKind : uint8_t
{
  Ignored,
  Context,
  ActionResult,
  Mute,
  Skip,
  Hangup
};

struct DataMessage
{
  std::string topic;
  std::string payload;
};

struct ClientMessage
{
  ClientMessageKind kind{ClientMessageKind::Ignored};
  argus::voice::v1::VoiceContext context;
  argus::voice::v1::VoiceActionResult actionResult;
  bool muted{false};
};

struct ClientPacket
{
  std::string_view topic;
  std::span<const uint8_t> payload;
};

[[nodiscard]] std::string agentStateToString(AgentState state);
[[nodiscard]] std::string doneReasonToString(DoneReason reason);
[[nodiscard]] std::optional<DataMessage> dataMessageOf(const argus::voice::v1::ServerFrame& frame);
[[nodiscard]] DataMessage doneMessage(DoneReason reason);
[[nodiscard]] ClientMessage clientMessageOf(const ClientPacket& packet);
[[nodiscard]] std::optional<int64_t> userIdOfIdentity(std::string_view identity);
[[nodiscard]] CallOutcome callOutcomeOf(const CallEnd& end);

}
