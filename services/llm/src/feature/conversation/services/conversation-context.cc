#include <feature/conversation/services/conversation-context.hxx>

#include <feature/conversation/vocabulary/conversation-turn-role.hxx>

#include <ranges>

ConversationContext buildConversationContext(const ConversationContextInput& input)
{
  ConversationContext context;
  for (const auto& message : input.requestMessages) {
    if (message.role == "system")
      context.messages.push_back(message);
  }
  for (const auto& turn : input.tail) {
    const std::string role = turn.role == ConversationTurnRole::User        ? "user"
                             : turn.role == ConversationTurnRole::Assistant ? "assistant"
                                                                            : "system";
    context.messages.push_back({.role = role, .content = turn.content});
  }
  for (const auto& message : std::views::reverse(input.requestMessages)) {
    if (message.role == "user") {
      context.latestUserText = message.content;
      context.messages.push_back({.role = "user", .content = message.content});
      break;
    }
  }
  return context;
}

ConversationContext openConversation(const ConversationStore& service, const ConversationOpenInput& input)
{
  (void)service.takeover({.userId = input.userId,
                          .candidate = input.leg,
                          .allowed = true,
                          .lang = input.lang,
                          .cameraPlaceWithArticle = {},
                          .at = input.at});
  const auto tail = service.contextTail(input.userId, input.tailTurns);
  return buildConversationContext({.requestMessages = input.requestMessages, .tail = tail});
}

bool recordExchange(const ConversationStore& service, const ConversationExchange& exchange)
{
  if (exchange.userText.empty() || exchange.reply.empty())
    return false;
  const bool user = service.recordTurn({.userId = exchange.userId,
                                        .role = ConversationTurnRole::User,
                                        .leg = exchange.leg,
                                        .content = exchange.userText,
                                        .at = exchange.at});
  const bool assistant = service.recordTurn({.userId = exchange.userId,
                                             .role = ConversationTurnRole::Assistant,
                                             .leg = exchange.leg,
                                             .content = exchange.reply,
                                             .at = exchange.at});
  return user && assistant;
}
