#include "rtc-wire.hxx"

#include <charconv>
#include <json/json.h>
#include <memory>
#include <voice/reaction-contracts.hxx>

namespace rtc_wire
{

namespace
{

std::string compact(const Json::Value& value)
{
  static const Json::StreamWriterBuilder builder = [] {
    Json::StreamWriterBuilder writer;
    writer["indentation"] = "";
    writer["emitUTF8"] = true;
    return writer;
  }();
  return Json::writeString(builder, value);
}

std::optional<Json::Value> parse(std::span<const uint8_t> payload)
{
  if (payload.empty() || payload.size() > kMaxClientPayloadBytes)
    return std::nullopt;
  static const Json::CharReaderBuilder builder;
  const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value value;
  const auto* begin = reinterpret_cast<const char*>(payload.data());
  if (!reader->parse(begin, begin + payload.size(), &value, nullptr) || !value.isObject())
    return std::nullopt;
  return value;
}

std::string stringOf(const Json::Value& payload, const char* key)
{
  const Json::Value& value = payload[key];
  return value.isString() ? value.asString() : std::string();
}

argus::voice::v1::VoiceContextKind contextKindOf(const Json::Value& payload)
{
  const std::string kind = stringOf(payload, "kind");
  if (kind == "cameraEvent")
    return argus::voice::v1::VOICE_CONTEXT_CAMERA_EVENT;
  if (kind == "situation")
    return argus::voice::v1::VOICE_CONTEXT_SITUATION;
  return argus::voice::v1::VOICE_CONTEXT_NOTE;
}

std::optional<int64_t> integerOf(const Json::Value& value)
{
  if (value.isIntegral())
    return value.asInt64();
  if (!value.isString())
    return std::nullopt;
  const std::string& raw = value.asString();
  int64_t parsed = 0;
  const auto [end, error] = std::from_chars(raw.data(), raw.data() + raw.size(), parsed);
  if (error != std::errc{} || end != raw.data() + raw.size())
    return std::nullopt;
  return parsed;
}

DataMessage messageOf(std::string_view topic, const Json::Value& payload)
{
  return {.topic = std::string(topic), .payload = compact(payload)};
}

}

std::string agentStateToString(AgentState state)
{
  switch (state) {
    case AgentState::Listening:
      return "listening";
    case AgentState::Thinking:
      return "thinking";
    case AgentState::Speaking:
      return "speaking";
    case AgentState::Initializing:
      break;
  }
  return "initializing";
}

std::string doneReasonToString(DoneReason reason)
{
  switch (reason) {
    case DoneReason::Timeout:
      return "timeout";
    case DoneReason::Revoked:
      return "revoked";
    case DoneReason::Error:
      return "error";
    case DoneReason::Hangup:
      break;
  }
  return "hangup";
}

std::optional<DataMessage> dataMessageOf(const argus::voice::v1::ServerFrame& frame)
{
  Json::Value payload(Json::objectValue);
  switch (frame.body_case()) {
    case argus::voice::v1::ServerFrame::kStt:
      payload["text"] = frame.stt().text();
      payload["final"] = frame.stt().final();
      return messageOf(kTopicStt, payload);
    case argus::voice::v1::ServerFrame::kAssistant:
      payload["text"] = frame.assistant().text();
      if (frame.assistant().turn_id() != 0)
        payload["turnId"] = static_cast<Json::Int64>(frame.assistant().turn_id());
      return messageOf(kTopicAssistant, payload);
    case argus::voice::v1::ServerFrame::kTurn:
      payload["id"] = static_cast<Json::Int64>(frame.turn().id());
      return messageOf(kTopicTurn, payload);
    case argus::voice::v1::ServerFrame::kInterrupted:
      payload["id"] = static_cast<Json::Int64>(frame.interrupted().id());
      return messageOf(kTopicInterrupted, payload);
    case argus::voice::v1::ServerFrame::kAction: {
      payload["id"] = static_cast<Json::Int64>(frame.action().id());
      payload["name"] = frame.action().name();
      const std::string& raw = frame.action().arguments();
      auto arguments = parse({reinterpret_cast<const uint8_t*>(raw.data()), raw.size()});
      payload["arguments"] = arguments ? *arguments : Json::Value(Json::objectValue);
      return messageOf(kTopicAction, payload);
    }
    case argus::voice::v1::ServerFrame::kEvent:
      payload["reaction"] =
          reactionKindToString(static_cast<ReactionKind>(frame.event().reaction()));
      payload["intensity"] = frame.event().intensity();
      payload["because"] = frame.event().because();
      return messageOf(kTopicEvent, payload);
    default:
      return std::nullopt;
  }
}

DataMessage doneMessage(DoneReason reason)
{
  Json::Value payload(Json::objectValue);
  payload["reason"] = doneReasonToString(reason);
  return messageOf(kTopicDone, payload);
}

DataMessage revokedMessage(std::string_view cause)
{
  Json::Value payload(Json::objectValue);
  payload["reason"] = doneReasonToString(DoneReason::Revoked);
  payload["cause"] = cause.empty() ? std::string("revoked") : std::string(cause);
  return messageOf(kTopicDone, payload);
}

ClientMessage clientMessageOf(const ClientPacket& packet)
{
  ClientMessage message;
  if (packet.topic == kTopicSkip) {
    message.kind = ClientMessageKind::Skip;
    return message;
  }
  if (packet.topic == kTopicHangup) {
    message.kind = ClientMessageKind::Hangup;
    return message;
  }
  const auto payload = parse(packet.payload);
  if (!payload)
    return message;
  if (packet.topic == kTopicContext) {
    message.kind = ClientMessageKind::Context;
    message.context.set_kind(contextKindOf(*payload));
    message.context.set_text(stringOf(*payload, "text"));
    message.context.set_camera(stringOf(*payload, "camera"));
  }
  else if (packet.topic == kTopicActionResult) {
    const auto id = integerOf((*payload)["id"]);
    if (!id)
      return message;
    message.kind = ClientMessageKind::ActionResult;
    message.actionResult.set_id(*id);
    message.actionResult.set_ok((*payload)["ok"].isBool() && (*payload)["ok"].asBool());
    message.actionResult.set_detail(stringOf(*payload, "detail"));
  }
  else if (packet.topic == kTopicMute) {
    message.kind = ClientMessageKind::Mute;
    message.muted = (*payload)["muted"].isBool() && (*payload)["muted"].asBool();
  }
  return message;
}

std::optional<int64_t> userIdOfIdentity(std::string_view identity)
{
  constexpr std::string_view prefix = "user:";
  if (!identity.starts_with(prefix))
    return std::nullopt;
  identity.remove_prefix(prefix.size());
  const auto colon = identity.find(':');
  if (colon == std::string_view::npos || colon == 0 || colon + 1 == identity.size())
    return std::nullopt;
  constexpr int64_t kMaxUserId = 1'000'000'000'000'000;
  int64_t userId = 0;
  for (const char c : identity.substr(0, colon)) {
    if (c < '0' || c > '9' || userId > kMaxUserId)
      return std::nullopt;
    userId = userId * 10 + (c - '0');
  }
  if (userId <= 0)
    return std::nullopt;
  return userId;
}

CallOutcome callOutcomeOf(const CallEnd& end)
{
  if (!end.callId.starts_with("call-"))
    return CallOutcome::NotReported;
  if (!end.userJoined)
    return CallOutcome::Failed;
  if (!end.openingSpoken)
    return end.reason == DoneReason::Error ? CallOutcome::Failed : CallOutcome::Declined;
  return CallOutcome::Completed;
}

}
