#include <doctest/doctest.h>

#include <feature/voice/services/leg-arbiter/conversation-route.hxx>

TEST_CASE("the voice leg values map to the conversation legs, and anything else is no leg")
{
  CHECK(legFromVoiceValue(1) == ConversationLegKind::Camera);
  CHECK(legFromVoiceValue(2) == ConversationLegKind::Phone);
  CHECK(legFromVoiceValue(3) == ConversationLegKind::Laptop);
  CHECK_FALSE(legFromVoiceValue(0).has_value());
  CHECK_FALSE(legFromVoiceValue(9).has_value());
}

TEST_CASE("the conversation route is off unless the flag is on")
{
  const auto route = conversationRouteFor({.flagEnabled = false, .userId = 7, .leg = ConversationLegKind::Phone});
  CHECK_FALSE(route.useConversation);
  CHECK_FALSE(route.leg.has_value());
}

TEST_CASE("the conversation route needs a user and a leg")
{
  CHECK_FALSE(conversationRouteFor({.flagEnabled = true, .userId = 0, .leg = ConversationLegKind::Phone})
                  .useConversation);
  CHECK_FALSE(conversationRouteFor({.flagEnabled = true, .userId = 7, .leg = std::nullopt}).useConversation);
}

TEST_CASE("a phone or laptop turn of a known user is routed to the conversation with its leg")
{
  const auto phone = conversationRouteFor({.flagEnabled = true, .userId = 7, .leg = ConversationLegKind::Phone});
  CHECK(phone.useConversation);
  CHECK(phone.leg == ConversationLegKind::Phone);
  const auto laptop = conversationRouteFor({.flagEnabled = true, .userId = 7, .leg = ConversationLegKind::Laptop});
  CHECK(laptop.useConversation);
  CHECK(laptop.leg == ConversationLegKind::Laptop);
}

TEST_CASE("a camera turn is not routed to the conversation until the watchman verifies it")
{
  const auto camera = conversationRouteFor({.flagEnabled = true, .userId = 7, .leg = ConversationLegKind::Camera});
  CHECK_FALSE(camera.useConversation);
  CHECK_FALSE(camera.leg.has_value());
}
