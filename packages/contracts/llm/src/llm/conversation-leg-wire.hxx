#pragma once

#include <llm/conversation-leg.hxx>

#include <cstdint>
#include <optional>

inline constexpr std::uint32_t kConversationLegUnspecified = 0;

inline std::uint32_t conversationLegWireValue(std::optional<ConversationLegKind> leg)
{
  return leg ? static_cast<std::uint32_t>(*leg) + 1 : kConversationLegUnspecified;
}

inline std::optional<ConversationLegKind> conversationLegFromWireValue(std::uint32_t value)
{
  if (value == kConversationLegUnspecified || value > 3)
    return std::nullopt;
  return static_cast<ConversationLegKind>(value - 1);
}
