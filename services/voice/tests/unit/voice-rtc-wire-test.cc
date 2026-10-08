#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include <doctest/doctest.h>

#include <feature/rtc/rtc-wire.hxx>

#include <json/json.h>

#include <memory>
#include <string>
#include <vector>

namespace
{

namespace v1 = argus::voice::v1;

Json::Value parsed(const std::string& text)
{
  Json::CharReaderBuilder builder;
  const std::unique_ptr<Json::CharReader> reader(builder.newCharReader());
  Json::Value value;
  REQUIRE(reader->parse(text.data(), text.data() + text.size(), &value, nullptr));
  return value;
}

rtc_wire::DataMessage messageOf(const v1::ServerFrame& frame)
{
  const auto message = rtc_wire::dataMessageOf(frame);
  REQUIRE(message.has_value());
  return message.value_or(rtc_wire::DataMessage{});
}

rtc_wire::ClientMessage clientOf(std::string_view topic, const std::string& payload)
{
  const std::vector<uint8_t> bytes(payload.begin(), payload.end());
  return rtc_wire::clientMessageOf({.topic = topic, .payload = bytes});
}

}

TEST_CASE("server frames become the data messages the app already understands")
{
  v1::ServerFrame stt;
  stt.mutable_stt()->set_text("¿qué tengo hoy?");
  stt.mutable_stt()->set_final(true);
  const auto sttMessage = messageOf(stt);
  CHECK(sttMessage.topic == "argus.stt");
  CHECK(parsed(sttMessage.payload)["text"].asString() == "¿qué tengo hoy?");
  CHECK(parsed(sttMessage.payload)["final"].asBool());

  v1::ServerFrame assistant;
  assistant.mutable_assistant()->set_text("Tienes dentista a las cinco.");
  assistant.mutable_assistant()->set_turn_id(3);
  const auto assistantMessage = messageOf(assistant);
  CHECK(assistantMessage.topic == "argus.assistant");
  CHECK(parsed(assistantMessage.payload)["turnId"].asInt64() == 3);

  v1::ServerFrame halfDuplex;
  halfDuplex.mutable_assistant()->set_text("Hola.");
  CHECK_FALSE(parsed(messageOf(halfDuplex).payload).isMember("turnId"));

  v1::ServerFrame turn;
  turn.mutable_turn()->set_id(4);
  CHECK(messageOf(turn).topic == "argus.turn");
  CHECK(parsed(messageOf(turn).payload)["id"].asInt64() == 4);

  v1::ServerFrame interrupted;
  interrupted.mutable_interrupted()->set_id(4);
  CHECK(messageOf(interrupted).topic == "argus.interrupted");

  v1::ServerFrame event;
  event.mutable_event()->set_reaction(v1::REACTION_WARM);
  event.mutable_event()->set_intensity(0.5F);
  event.mutable_event()->set_because("greeting");
  const Json::Value eventPayload = parsed(messageOf(event).payload);
  CHECK(messageOf(event).topic == "argus.event");
  CHECK(eventPayload["reaction"].asString() == "warm");
  CHECK(eventPayload["because"].asString() == "greeting");
}

TEST_CASE("an action keeps its arguments as an object, and an unparsable one becomes {}")
{
  v1::ServerFrame action;
  action.mutable_action()->set_id(5);
  action.mutable_action()->set_name("app.show_camera");
  action.mutable_action()->set_arguments(R"({"camera":"Patio"})");
  const Json::Value payload = parsed(messageOf(action).payload);
  CHECK(payload["id"].asInt64() == 5);
  CHECK(payload["name"].asString() == "app.show_camera");
  CHECK(payload["arguments"]["camera"].asString() == "Patio");

  action.mutable_action()->set_arguments("not json");
  CHECK(parsed(messageOf(action).payload)["arguments"].isObject());
  CHECK(parsed(messageOf(action).payload)["arguments"].empty());
}

TEST_CASE("audio and done frames never become data messages")
{
  v1::ServerFrame chunk;
  chunk.mutable_tts_chunk()->set_pcm(std::string(320, '\0'));
  CHECK_FALSE(rtc_wire::dataMessageOf(chunk));
  v1::ServerFrame done;
  done.mutable_done()->set_session_id(1);
  CHECK_FALSE(rtc_wire::dataMessageOf(done));
  CHECK(parsed(rtc_wire::doneMessage(rtc_wire::DoneReason::Revoked).payload)["reason"].asString() == "revoked");
  CHECK(rtc_wire::doneMessage(rtc_wire::DoneReason::Hangup).topic == "argus.done");
  const Json::Value revoked = parsed(rtc_wire::revokedMessage("accountDisabled").payload);
  CHECK(revoked["reason"].asString() == "revoked");
  CHECK(revoked["cause"].asString() == "accountDisabled");
  CHECK(parsed(rtc_wire::revokedMessage("").payload)["cause"].asString() == "revoked");
}

TEST_CASE("client data messages map onto the session's inputs")
{
  const auto context = clientOf("argus.context", R"({"kind":"cameraEvent","text":"Una persona","camera":"Patio"})");
  CHECK(context.kind == rtc_wire::ClientMessageKind::Context);
  CHECK(context.context.kind() == v1::VOICE_CONTEXT_CAMERA_EVENT);
  CHECK(context.context.camera() == "Patio");

  CHECK(clientOf("argus.context", R"({"kind":"situation","text":"x"})").context.kind() ==
        v1::VOICE_CONTEXT_SITUATION);
  CHECK(clientOf("argus.context", R"({"kind":"whatever","text":"x"})").context.kind() ==
        v1::VOICE_CONTEXT_NOTE);

  const auto result = clientOf("argus.action_result", R"({"id":"7","ok":false,"detail":"sin permiso"})");
  CHECK(result.kind == rtc_wire::ClientMessageKind::ActionResult);
  CHECK(result.actionResult.id() == 7);
  CHECK_FALSE(result.actionResult.ok());
  CHECK(result.actionResult.detail() == "sin permiso");
  CHECK(clientOf("argus.action_result", R"({"id":"x7","ok":true})").kind == rtc_wire::ClientMessageKind::Ignored);
  CHECK_FALSE(clientOf("argus.action_result", R"({"id":3,"ok":"true"})").actionResult.ok());

  CHECK(clientOf("argus.mute", R"({"muted":true})").muted);
  CHECK_FALSE(clientOf("argus.mute", R"({"muted":"yes"})").muted);
  CHECK(clientOf("argus.skip", "").kind == rtc_wire::ClientMessageKind::Skip);
  CHECK(clientOf("argus.hangup", "").kind == rtc_wire::ClientMessageKind::Hangup);
  CHECK(clientOf("argus.unknown", "{}").kind == rtc_wire::ClientMessageKind::Ignored);
  CHECK(clientOf("argus.context", "[1,2]").kind == rtc_wire::ClientMessageKind::Ignored);
  CHECK(clientOf("argus.context", std::string(rtc_wire::kMaxClientPayloadBytes + 1, ' ')).kind ==
        rtc_wire::ClientMessageKind::Ignored);
}

TEST_CASE("the user id comes only from a well-formed participant identity")
{
  CHECK(rtc_wire::userIdOfIdentity("user:7:9c1e") == 7);
  CHECK_FALSE(rtc_wire::userIdOfIdentity("user:7:"));
  CHECK_FALSE(rtc_wire::userIdOfIdentity("user::abc"));
  CHECK_FALSE(rtc_wire::userIdOfIdentity("user:-3:abc"));
  CHECK_FALSE(rtc_wire::userIdOfIdentity("user:7x:abc"));
  CHECK_FALSE(rtc_wire::userIdOfIdentity("argus-voice"));
}

TEST_CASE("only proactive calls report an outcome, and leaving before the opening line is a decline")
{
  using rtc_wire::CallOutcome;
  using rtc_wire::DoneReason;
  CHECK(rtc_wire::callOutcomeOf({.callId = "rtc-00", .reason = DoneReason::Hangup, .userJoined = true, .openingSpoken = true}) ==
        CallOutcome::NotReported);
  CHECK(rtc_wire::callOutcomeOf({.callId = "call-4", .reason = DoneReason::Hangup, .userJoined = true, .openingSpoken = true}) ==
        CallOutcome::Completed);
  CHECK(rtc_wire::callOutcomeOf({.callId = "call-4", .reason = DoneReason::Hangup, .userJoined = true, .openingSpoken = false}) ==
        CallOutcome::Declined);
  CHECK(rtc_wire::callOutcomeOf({.callId = "call-4", .reason = DoneReason::Error, .userJoined = true, .openingSpoken = false}) ==
        CallOutcome::Failed);
  CHECK(rtc_wire::callOutcomeOf({.callId = "call-4", .reason = DoneReason::Timeout, .userJoined = false, .openingSpoken = false}) ==
        CallOutcome::Failed);
}

TEST_CASE("agent states use LiveKit's agent vocabulary")
{
  CHECK(rtc_wire::agentStateToString(rtc_wire::AgentState::Initializing) == "initializing");
  CHECK(rtc_wire::agentStateToString(rtc_wire::AgentState::Listening) == "listening");
  CHECK(rtc_wire::agentStateToString(rtc_wire::AgentState::Thinking) == "thinking");
  CHECK(rtc_wire::agentStateToString(rtc_wire::AgentState::Speaking) == "speaking");
}

TEST_CASE("a call with nothing to say starts listening, one with an opening starts thinking")
{
  CHECK(rtc_wire::agentStateForOpening(true) == rtc_wire::AgentState::Thinking);
  CHECK(rtc_wire::agentStateForOpening(false) == rtc_wire::AgentState::Listening);
  CHECK(rtc_wire::agentStateToString(rtc_wire::agentStateForOpening(true)) == "thinking");
  CHECK(rtc_wire::agentStateToString(rtc_wire::agentStateForOpening(false)) == "listening");
}
