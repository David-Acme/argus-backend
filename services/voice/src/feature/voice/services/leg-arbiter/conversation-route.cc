#include <feature/voice/services/leg-arbiter/conversation-route.hxx>

std::optional<ConversationLegKind> legFromVoiceValue(uint32_t value)
{
  switch (value) {
    case 1:
      return ConversationLegKind::Camera;
    case 2:
      return ConversationLegKind::Phone;
    case 3:
      return ConversationLegKind::Laptop;
    default:
      return std::nullopt;
  }
}

ConversationRoute conversationRouteFor(const ConversationRouteInput& input)
{
  const bool routable = input.flagEnabled && input.userId > 0 && input.leg.has_value() &&
                        *input.leg != ConversationLegKind::Camera;
  return {.useConversation = routable, .leg = routable ? input.leg : std::nullopt};
}
