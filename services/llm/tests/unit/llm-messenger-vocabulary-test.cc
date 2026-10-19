#include <doctest/doctest.h>

#include <feature/messenger/vocabulary/messenger-errors.hxx>
#include <feature/messenger/vocabulary/messenger-settings.hxx>
#include <feature/messenger/vocabulary/relay-outcome.hxx>

TEST_CASE("every relay channel round-trips through its wire name")
{
  for (const auto channel : {RelayChannel::Conversation, RelayChannel::Notification, RelayChannel::Camera}) {
    const auto name = relayChannelToString(channel);
    REQUIRE(relayChannelFromString(name).has_value());
    CHECK(relayChannelFromString(name).value() == channel);
  }
  CHECK(relayChannelToString(RelayChannel::Conversation) == "conversation");
  CHECK(relayChannelToString(RelayChannel::Notification) == "notification");
  CHECK(relayChannelToString(RelayChannel::Camera) == "camera");
}

TEST_CASE("a channel name the build does not know is refused, never defaulted")
{
  CHECK_FALSE(relayChannelFromString("ring").has_value());
  CHECK_FALSE(relayChannelFromString("").has_value());
  CHECK_FALSE(relayChannelFromString("Conversation").has_value());
}

TEST_CASE("every relay outcome round-trips and uses the snake_case wire names")
{
  for (const auto outcome : {RelayOutcome::Delivered, RelayOutcome::Notified, RelayOutcome::NoAnswer,
                             RelayOutcome::NoRecipient, RelayOutcome::Unavailable}) {
    const auto name = relayOutcomeToString(outcome);
    REQUIRE(relayOutcomeFromString(name).has_value());
    CHECK(relayOutcomeFromString(name).value() == outcome);
  }
  CHECK(relayOutcomeToString(RelayOutcome::NoAnswer) == "no_answer");
  CHECK(relayOutcomeToString(RelayOutcome::NoRecipient) == "no_recipient");
}

TEST_CASE("an outcome name the build does not know is refused")
{
  CHECK_FALSE(relayOutcomeFromString("ringing").has_value());
  CHECK_FALSE(relayOutcomeFromString("noanswer").has_value());
}

TEST_CASE("the refusals of the messenger carry their status and the wire code they map to")
{
  CHECK(MessengerErrors::RecipientNotFound.status == 404);
  CHECK(MessengerErrors::MessageTooLong.status == 400);
  CHECK(MessengerErrors::ChannelUnavailable.status == 503);
  CHECK(MessengerErrors::RecipientNotFound.wireCode() == "NOT_FOUND");
}

TEST_CASE("the messenger's defaults are a 300 s reply window, 20 relays an hour and 500 characters")
{
  const MessengerSettings settings;
  CHECK(settings.enabled);
  CHECK(settings.replyWindowSeconds == 300);
  CHECK(settings.maxRelaysPerHour == 20);
  CHECK(settings.maxMessageChars == 500);
  CHECK(settings.maxVisitorNoticesPerHourPerCamera == 6);
}
