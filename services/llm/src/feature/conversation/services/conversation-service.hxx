#pragma once

#include <feature/conversation/repositories/conversation/conversation-repository.hxx>
#include <feature/conversation/services/conversation-store.hxx>
#include <feature/conversation/vocabulary/conversation-turn-role.hxx>
#include <llm/conversation-leg.hxx>

#include <cstdint>
#include <mutex>
#include <string>
#include <vector>

struct sqlite3;

struct ConversationPolicy
{
  int64_t idleSeconds{900};
};

struct ConversationServiceDeps
{
  sqlite3* db;
  std::mutex& mutex;
  ConversationPolicy policy;
};

struct InboundRequest
{
  int64_t userId{0};
  std::string content;
  int64_t at{0};
};

class ConversationService final : public ConversationStore
{
public:
  explicit ConversationService(ConversationServiceDeps deps);

  [[nodiscard]] bool recordTurn(const TurnRecord& turn) const override;

  [[nodiscard]] TakeoverResult takeover(const TakeoverRequest& request) const override;

  [[nodiscard]] bool hasOpenConversation(int64_t userId, int64_t now) const;

  [[nodiscard]] bool appendInbound(const InboundRequest& inbound) const;

  [[nodiscard]] std::vector<int64_t> idleUsers(int64_t now) const;

  [[nodiscard]] std::vector<ConversationTurnRow> contextTail(int64_t userId, int64_t limit) const override;

  [[nodiscard]] int purgeExpired(int64_t now, int64_t retentionDays) const;

private:
  [[nodiscard]] bool isOpen(const ConversationRow& row, int64_t now) const;

  sqlite3* db_;
  std::mutex& mutex_;
  ConversationPolicy policy_;
  ConversationRepository repository_;
};
