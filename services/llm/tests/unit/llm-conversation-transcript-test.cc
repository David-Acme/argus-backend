#include <doctest/doctest.h>

#include <feature/conversation/infra/notification-conversation-transcript.hxx>

TEST_CASE("only an identified exchange with words on both sides is mirrored")
{
  CHECK(transcribable({.userId = 7, .leg = ConversationLegKind::Phone, .userText = "hola", .reply = "buenas", .at = 1}));
  CHECK_FALSE(transcribable({.userId = 0, .leg = std::nullopt, .userText = "hola", .reply = "buenas", .at = 1}));
  CHECK_FALSE(transcribable({.userId = 7, .leg = std::nullopt, .userText = "  ", .reply = "buenas", .at = 1}));
  CHECK_FALSE(transcribable({.userId = 7, .leg = std::nullopt, .userText = "hola", .reply = "", .at = 1}));
}

TEST_CASE("the mirrored turns name the speaker, the leg and the expiry")
{
  const auto request = appendTurnsOf(
      {.userId = 7, .leg = ConversationLegKind::Laptop, .userText = "hola", .reply = "buenas", .at = 100}, 500);
  CHECK(request.user_id() == 7);
  CHECK(request.leg() == "laptop");
  CHECK(request.expires_at() == 500);
  REQUIRE(request.turns_size() == 2);
  CHECK(request.turns(0).speaker() == argus::notification::v1::TURN_SPEAKER_USER);
  CHECK(request.turns(0).body() == "hola");
  CHECK(request.turns(1).speaker() == argus::notification::v1::TURN_SPEAKER_ASSISTANT);
  CHECK(request.turns(1).body() == "buenas");

  const auto legless =
      appendTurnsOf({.userId = 7, .leg = std::nullopt, .userText = "hola", .reply = "buenas", .at = 100}, 0);
  CHECK(legless.leg().empty());
}
