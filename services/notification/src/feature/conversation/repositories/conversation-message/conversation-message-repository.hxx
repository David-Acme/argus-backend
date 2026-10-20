#pragma once

#include "conversation-message-query.hxx"

#include <drogon/utils/coroutine.h>
#include <optional>
#include <vector>

class ConversationMessageRepository
{
public:
  [[nodiscard]] drogon::Task<ConversationCopiesResult> insertCopies(const ConversationCopiesInput& input) const;

  [[nodiscard]] drogon::Task<std::optional<ConversationMessageSchema>> findCopy(const ConversationCopyRef& ref) const;

  [[nodiscard]] drogon::Task<int64_t> countSentSince(const ConversationSentSince& input) const;

  [[nodiscard]] drogon::Task<std::optional<ConversationMessageSchema>>
  findThreadLast(const ConversationThreadKey& key) const;

  [[nodiscard]] drogon::Task<std::vector<ConversationMessageChange>>
  transition(const ConversationTransitionInput& input) const;

  [[nodiscard]] drogon::Task<std::vector<ConversationMessageChange>>
  markThreadRead(const ConversationThreadInput& input) const;

  [[nodiscard]] drogon::Task<std::vector<ConversationMessageChange>>
  removeThread(const ConversationThreadInput& input) const;

  [[nodiscard]] drogon::Task<std::vector<ConversationThreadSummary>> threads(int64_t userId) const;

  [[nodiscard]] drogon::Task<std::vector<ConversationMessageSchema>> findSync(const ConversationSyncFilter& filter) const;

  [[nodiscard]] drogon::Task<std::optional<ConversationMessageSchema>> findLastSync(int64_t userId) const;

  [[nodiscard]] drogon::Task<std::vector<ConversationMessageSchema>>
  findDeletedSync(const ConversationSyncFilter& filter) const;

  [[nodiscard]] drogon::Task<std::optional<ConversationMessageSchema>> findLastDeletedSync(int64_t userId) const;

  [[nodiscard]] drogon::Task<std::vector<ConversationMessageSchema>> expire(const ConversationExpireInput& input) const;

  drogon::Task<void> purgeDeleted(int64_t before) const;
};
