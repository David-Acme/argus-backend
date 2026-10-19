#pragma once

#include <feature/conversation/repositories/conversation/conversation-query.hxx>
#include <feature/conversation/vocabulary/conversation-turn-role.hxx>
#include <llm/conversation-leg.hxx>

#include <cstdint>
#include <string>
#include <vector>

struct TurnRecord
{
  int64_t userId{0};
  ConversationTurnRole role{ConversationTurnRole::User};
  ConversationLegKind leg{ConversationLegKind::Phone};
  std::string content;
  int64_t at{0};
};

struct TakeoverRequest
{
  int64_t userId{0};
  ConversationLegKind candidate{ConversationLegKind::Phone};
  bool allowed{false};
  std::string lang;
  std::string cameraPlaceWithArticle;
  int64_t at{0};
};

struct TakeoverResult
{
  bool switched{false};
  std::string note;
};

class ConversationStore
{
public:
  ConversationStore() = default;
  virtual ~ConversationStore() = default;

  ConversationStore(const ConversationStore&) = delete;
  ConversationStore& operator=(const ConversationStore&) = delete;

  [[nodiscard]] virtual bool recordTurn(const TurnRecord& turn) const = 0;

  [[nodiscard]] virtual TakeoverResult takeover(const TakeoverRequest& request) const = 0;

  [[nodiscard]] virtual std::vector<ConversationTurnRow> contextTail(int64_t userId, int64_t limit) const = 0;
};
