#pragma once

#include <feature/conversation/repositories/conversation-message/conversation-message-query.hxx>
#include <json/value.h>

#include <vector>

struct ResponseConversationThreadsDto
{
  std::vector<ConversationThreadSummary> threads;

  [[nodiscard]] Json::Value toJson() const;
};
