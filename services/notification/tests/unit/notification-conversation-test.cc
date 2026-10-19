#include <doctest/doctest.h>

#include <app/rpc/conversation-rpc-service.hxx>
#include <feature/conversation/dtos/response-conversation-threads-dto.hxx>
#include <feature/conversation/services/conversation-inbox-service.hxx>

#include <string>

namespace
{
namespace v1 = argus::notification::v1;

RelayLetter relayFromAna()
{
  return {.fromUserId = 3,
          .fromName = "Ana",
          .toUserId = 7,
          .toName = "David",
          .kind = MessageKind::Relay,
          .body = "que venga a comer",
          .spokenLine = "Ana te manda decir: que venga a comer",
          .title = "Ana",
          .replyToKey = std::nullopt,
          .commandId = "relay-3-100-1-7",
          .expiresAt = 0};
}

v1::RelayMessageRequest requestFrom(const RelayLetter& letter)
{
  v1::RelayMessageRequest request;
  request.set_from_user_id(letter.fromUserId);
  request.set_from_name(letter.fromName);
  request.set_to_user_id(letter.toUserId);
  request.set_to_name(letter.toName);
  request.set_kind(v1::MESSAGE_KIND_RELAY);
  request.set_body(letter.body);
  request.set_spoken_line(letter.spokenLine);
  request.set_title(letter.title);
  request.set_command_id(letter.commandId);
  return request;
}
}

TEST_CASE("a relay letter needs two different members, the sender's words and a key")
{
  CHECK(validLetter(relayFromAna()));

  RelayLetter self = relayFromAna();
  self.toUserId = self.fromUserId;
  CHECK_FALSE(validLetter(self));

  RelayLetter empty = relayFromAna();
  empty.body = "   ";
  CHECK_FALSE(validLetter(empty));

  RelayLetter keyless = relayFromAna();
  keyless.commandId.clear();
  CHECK_FALSE(validLetter(keyless));

  RelayLetter turn = relayFromAna();
  turn.kind = MessageKind::Turn;
  CHECK_FALSE(validLetter(turn));

  RelayLetter longBody = relayFromAna();
  longBody.body = std::string(ConversationInboxService::kMaxBodyBytes + 1, 'a');
  CHECK_FALSE(validLetter(longBody));
}

TEST_CASE("a visitor letter has no sender account and a member one always has one")
{
  RelayLetter visitor = relayFromAna();
  visitor.kind = MessageKind::Visitor;
  visitor.fromUserId = 0;
  visitor.fromName.clear();
  CHECK(validLetter(visitor));

  RelayLetter posing = relayFromAna();
  posing.kind = MessageKind::Visitor;
  CHECK_FALSE(validLetter(posing));

  RelayLetter anonymous = relayFromAna();
  anonymous.fromUserId = 0;
  CHECK_FALSE(validLetter(anonymous));
}

TEST_CASE("a reply names the message it answers")
{
  RelayLetter reply = relayFromAna();
  reply.kind = MessageKind::Reply;
  CHECK_FALSE(validLetter(reply));
  reply.replyToKey = "relay-7-90-1-3";
  CHECK(validLetter(reply));
}

TEST_CASE("the relay request maps onto the letter and refuses an unknown kind")
{
  const RelayLetter expected = relayFromAna();
  const auto letter = relayLetterOf(requestFrom(expected));
  REQUIRE(letter.has_value());
  CHECK(letter->fromUserId == 3);
  CHECK(letter->toUserId == 7);
  CHECK(letter->kind == MessageKind::Relay);
  CHECK(letter->body == expected.body);
  CHECK_FALSE(letter->replyToKey.has_value());

  v1::RelayMessageRequest unknown = requestFrom(expected);
  unknown.set_kind(v1::MESSAGE_KIND_UNSPECIFIED);
  CHECK_FALSE(relayLetterOf(unknown).has_value());
}

TEST_CASE("a settlement is only answered or expired")
{
  v1::SettleMessageRequest request;
  request.set_message_key("relay-3-100-1-7");
  request.set_settlement(v1::MESSAGE_SETTLEMENT_EXPIRED);
  const auto expired = settleLetterOf(request);
  REQUIRE(expired.has_value());
  CHECK(expired->state == MessageState::Expired);

  request.set_settlement(v1::MESSAGE_SETTLEMENT_UNSPECIFIED);
  CHECK_FALSE(settleLetterOf(request).has_value());

  request.set_settlement(v1::MESSAGE_SETTLEMENT_ANSWERED);
  request.set_message_key("");
  CHECK_FALSE(settleLetterOf(request).has_value());
}

TEST_CASE("the turns of a conversation keep who spoke and the leg they were said on")
{
  v1::AppendTurnsRequest request;
  request.set_user_id(7);
  request.set_leg("phone");
  request.set_command_id("turn-7-100");
  auto* said = request.add_turns();
  said->set_speaker(v1::TURN_SPEAKER_USER);
  said->set_body("hola");
  auto* answered = request.add_turns();
  answered->set_speaker(v1::TURN_SPEAKER_ASSISTANT);
  answered->set_body("buenas");

  const auto input = transcriptOf(request);
  REQUIRE(input.has_value());
  CHECK(input->leg == ConversationLegKind::Phone);
  REQUIRE(input->lines.size() == 2);
  CHECK(input->lines[0].direction == MessageDirection::Out);
  CHECK(input->lines[1].direction == MessageDirection::In);

  request.set_leg("tablet");
  CHECK_FALSE(transcriptOf(request).has_value());

  request.set_leg("");
  request.set_user_id(0);
  CHECK_FALSE(transcriptOf(request).has_value());
}

TEST_CASE("a conversation row serialises in the app's camelCase shape")
{
  ConversationMessageSchema row;
  row.id = 11;
  row.userId = 3;
  row.counterpartId = 7;
  row.counterpartName = "David";
  row.direction = MessageDirection::Out;
  row.kind = MessageKind::Relay;
  row.body = "que venga a comer";
  row.state = MessageState::Notified;
  row.messageKey = "relay-3-100-1-7";
  row.isRead = true;
  row.readAt = 100;
  row.createdAt = 100;
  row.expiresAt = 500;

  const Json::Value json = row.toJson();
  CHECK(json["counterpartId"].asInt64() == 7);
  CHECK(json["direction"].asString() == "out");
  CHECK(json["kind"].asString() == "relay");
  CHECK(json["state"].asString() == "notified");
  CHECK(json["leg"].isNull());
  CHECK(json["replyToKey"].isNull());
  CHECK(json["deletedAt"].isNull());
  CHECK_FALSE(json.isMember("expiresAt"));

  const ResponseConversationThreadsDto threads{.threads = {{.last = row, .unread = 2}}};
  const Json::Value list = threads.toJson();
  REQUIRE(list.size() == 1);
  CHECK(list[0]["counterpartId"].asInt64() == 7);
  CHECK(list[0]["counterpartName"].asString() == "David");
  CHECK(list[0]["unread"].asInt64() == 2);
  CHECK(list[0]["last"]["body"].asString() == "que venga a comer");
}
