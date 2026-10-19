#include <doctest/doctest.h>
#include <notification/message-direction.hxx>
#include <notification/message-kind.hxx>
#include <notification/message-state.hxx>
#include <string>
#include <utility>
#include <vector>

TEST_CASE("message directions round-trip through the column spelling")
{
  const std::vector<std::pair<MessageDirection, std::string>> pairs{{MessageDirection::Out, "out"},
                                                                    {MessageDirection::In, "in"}};
  for (const auto& [direction, name] : pairs) {
    CAPTURE(name);
    CHECK(messageDirectionToString(direction) == name);
    CHECK(messageDirectionFromString(name) == direction);
  }
  CHECK_FALSE(messageDirectionFromString("OUT").has_value());
  CHECK_FALSE(messageDirectionFromString("").has_value());
}

TEST_CASE("message kinds round-trip through the column spelling")
{
  const std::vector<std::pair<MessageKind, std::string>> pairs{{MessageKind::Turn, "turn"},
                                                               {MessageKind::Relay, "relay"},
                                                               {MessageKind::Reply, "reply"},
                                                               {MessageKind::Visitor, "visitor"}};
  for (const auto& [kind, name] : pairs) {
    CAPTURE(name);
    CHECK(messageKindToString(kind) == name);
    CHECK(messageKindFromString(name) == kind);
  }
  CHECK_FALSE(messageKindFromString("call").has_value());
}

TEST_CASE("message states round-trip and an unknown name reads as nothing")
{
  const std::vector<std::pair<MessageState, std::string>> pairs{
      {MessageState::Sent, "sent"},         {MessageState::Delivered, "delivered"},
      {MessageState::Notified, "notified"}, {MessageState::Answered, "answered"},
      {MessageState::Expired, "expired"},   {MessageState::Received, "received"}};
  for (const auto& [state, name] : pairs) {
    CAPTURE(name);
    CHECK(messageStateToString(state) == name);
    CHECK(messageStateFromString(name) == state);
  }
  CHECK_FALSE(messageStateFromString("read").has_value());
  CHECK_FALSE(messageStateFromString("Delivered").has_value());
}
