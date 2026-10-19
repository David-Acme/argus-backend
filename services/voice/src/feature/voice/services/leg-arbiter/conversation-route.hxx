#pragma once

#include <llm/conversation-leg.hxx>

#include <cstdint>
#include <optional>

struct ConversationRouteInput
{
  bool flagEnabled{false};
  int64_t userId{0};
  std::optional<ConversationLegKind> leg;
};

struct ConversationRoute
{
  bool useConversation{false};
  std::optional<ConversationLegKind> leg;
};

[[nodiscard]] std::optional<ConversationLegKind> legFromVoiceValue(uint32_t value);

[[nodiscard]] ConversationRoute conversationRouteFor(const ConversationRouteInput& input);
