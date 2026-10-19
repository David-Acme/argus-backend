#include <doctest/doctest.h>

#include <feature/messenger/infra/notification-message-courier.hxx>

TEST_CASE("a courier letter maps onto the relay request field by field")
{
  const auto request = relayRequestOf({.fromUserId = 1,
                                       .fromName = "David",
                                       .toUserId = 2,
                                       .toName = "Ana",
                                       .kind = messenger::CourierKind::Reply,
                                       .body = "ya voy",
                                       .spokenLine = "Ana te manda decir: ya voy",
                                       .title = "Ana",
                                       .replyToKey = "relay-1-9",
                                       .commandId = "reply-2-10-1-1",
                                       .expiresAt = 900});
  CHECK(request.from_user_id() == 1);
  CHECK(request.to_user_id() == 2);
  CHECK(request.kind() == argus::notification::v1::MESSAGE_KIND_REPLY);
  CHECK(request.body() == "ya voy");
  CHECK(request.reply_to_key() == "relay-1-9");
  CHECK(request.command_id() == "reply-2-10-1-1");
  CHECK(request.expires_at() == 900);
}

TEST_CASE("without a notification client nothing is carried and nothing is settled")
{
  const NotificationMessageCourier courier(nullptr);
  CHECK_FALSE(courier.carry({}).carried);
  CHECK_FALSE(courier.settle({.messageKey = "k", .settlement = messenger::CourierSettlement::Expired}));
}
