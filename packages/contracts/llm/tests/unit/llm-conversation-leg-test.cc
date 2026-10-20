#include <doctest/doctest.h>
#include <llm/conversation-leg.hxx>

#include <array>
#include <test-support/value-of.hxx>

namespace
{
constexpr std::array kAllLegs{ConversationLegKind::Camera, ConversationLegKind::Phone,
                              ConversationLegKind::Laptop};
}

TEST_CASE("every leg kind names itself and parses back to itself")
{
  for (const ConversationLegKind kind : kAllLegs) {
    const auto parsed = valueOf(conversationLegKindFromString(conversationLegKindToString(kind)));
    CHECK(parsed == kind);
  }
}

TEST_CASE("leg names are the lowercase wire spellings")
{
  CHECK(conversationLegKindToString(ConversationLegKind::Camera) == "camera");
  CHECK(conversationLegKindToString(ConversationLegKind::Phone) == "phone");
  CHECK(conversationLegKindToString(ConversationLegKind::Laptop) == "laptop");
}

TEST_CASE("a name outside the enum answers nothing")
{
  CHECK_FALSE(conversationLegKindFromString("Phone").has_value());
  CHECK_FALSE(conversationLegKindFromString("tablet").has_value());
  CHECK_FALSE(conversationLegKindFromString("").has_value());
}
