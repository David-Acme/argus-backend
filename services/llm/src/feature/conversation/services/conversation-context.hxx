#pragma once

#include <feature/conversation/repositories/conversation/conversation-query.hxx>
#include <feature/conversation/services/conversation-store.hxx>
#include <llm/conversation-leg.hxx>
#include <cstdint>

#include <string>
#include <vector>

struct ConversationMessage
{
  std::string role;
  std::string content;
};

struct ConversationContextInput
{
  std::vector<ConversationMessage> requestMessages;
  std::vector<ConversationTurnRow> tail;
};

struct ConversationContext
{
  std::vector<ConversationMessage> messages;
  std::string latestUserText;
};

[[nodiscard]] ConversationContext buildConversationContext(const ConversationContextInput& input);

struct ConversationOpenInput
{
  int64_t userId{0};
  ConversationLegKind leg{ConversationLegKind::Phone};
  std::string lang;
  int64_t at{0};
  int tailTurns{0};
  std::vector<ConversationMessage> requestMessages;
};

struct ConversationExchange
{
  int64_t userId{0};
  ConversationLegKind leg{ConversationLegKind::Phone};
  std::string userText;
  std::string reply;
  int64_t at{0};
};

[[nodiscard]] ConversationContext openConversation(const ConversationStore& service,
                                                   const ConversationOpenInput& input);

[[nodiscard]] bool recordExchange(const ConversationStore& service, const ConversationExchange& exchange);
