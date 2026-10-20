#include <doctest/doctest.h>
#include <llm/conversation-leg-wire.hxx>
#include <test-support/value-of.hxx>

TEST_CASE("an unspecified leg is the wire zero and reads back as no leg")
{
  CHECK(conversationLegWireValue(std::nullopt) == kConversationLegUnspecified);
  CHECK_FALSE(conversationLegFromWireValue(kConversationLegUnspecified).has_value());
}

TEST_CASE("every leg kind has a wire value in 1 to 3 and reads back to itself")
{
  for (const ConversationLegKind kind :
       {ConversationLegKind::Camera, ConversationLegKind::Phone, ConversationLegKind::Laptop}) {
    const std::uint32_t value = conversationLegWireValue(kind);
    CHECK(value >= 1);
    CHECK(value <= 3);
    const auto back = valueOf(conversationLegFromWireValue(value));
    CHECK(back == kind);
  }
}

TEST_CASE("a wire value outside the enum reads as no leg")
{
  CHECK_FALSE(conversationLegFromWireValue(4).has_value());
  CHECK_FALSE(conversationLegFromWireValue(99).has_value());
}
