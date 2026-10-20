#include <feature/conversation/services/conversation-service.hxx>

#include <feature/conversation/services/switch-note.hxx>

#include <sqlite3.h>

#include <utility>

ConversationService::ConversationService(ConversationServiceDeps deps)
    : db_(deps.db), mutex_(deps.mutex), policy_(deps.policy)
{
}

bool ConversationService::isOpen(const ConversationRow& row, int64_t now) const
{
  return row.lastTurnAt > 0 && now - row.lastTurnAt <= policy_.idleSeconds;
}

bool ConversationService::recordTurn(const TurnRecord& turn) const
{
  std::scoped_lock lock(mutex_);
  if (!repository_.touchLeg(db_, {.userId = turn.userId, .leg = turn.leg, .at = turn.at}))
    return false;
  return repository_.appendTurn(db_, {.userId = turn.userId,
                                      .role = turn.role,
                                      .leg = turn.leg,
                                      .content = turn.content,
                                      .at = turn.at});
}

TakeoverResult ConversationService::takeover(const TakeoverRequest& request) const
{
  if (!request.allowed)
    return {.switched = false, .note = {}};
  std::scoped_lock lock(mutex_);
  const auto before = repository_.find(db_, request.userId);
  if (!repository_.touchLeg(db_, {.userId = request.userId, .leg = request.candidate, .at = request.at}))
    return {.switched = false, .note = {}};
  if (!before || before->activeLeg == request.candidate)
    return {.switched = false, .note = {}};
  std::string note = switchNote({.lang = request.lang,
                                 .from = before->activeLeg,
                                 .to = request.candidate,
                                 .cameraPlaceWithArticle = request.cameraPlaceWithArticle});
  if (!repository_.appendTurn(db_, {.userId = request.userId,
                                    .role = ConversationTurnRole::Note,
                                    .leg = request.candidate,
                                    .content = note,
                                    .at = request.at}))
    return {.switched = false, .note = {}};
  return {.switched = true, .note = std::move(note)};
}

bool ConversationService::hasOpenConversation(const OpenConversationQuery& query) const
{
  std::scoped_lock lock(mutex_);
  const auto row = repository_.find(db_, query.userId);
  return row && isOpen(*row, query.now);
}

bool ConversationService::appendInbound(const InboundRequest& inbound) const
{
  std::scoped_lock lock(mutex_);
  const auto row = repository_.find(db_, inbound.userId);
  if (!row || !isOpen(*row, inbound.at))
    return false;
  return repository_.appendTurn(db_, {.userId = inbound.userId,
                                      .role = ConversationTurnRole::Note,
                                      .leg = row->activeLeg,
                                      .content = inbound.content,
                                      .at = inbound.at});
}

std::vector<int64_t> ConversationService::idleUsers(int64_t now) const
{
  std::scoped_lock lock(mutex_);
  return repository_.idleBefore(db_, now - policy_.idleSeconds);
}

std::vector<ConversationTurnRow> ConversationService::contextTail(int64_t userId, int64_t limit) const
{
  std::scoped_lock lock(mutex_);
  return repository_.recentTurns(db_, {.userId = userId, .limit = limit});
}

int ConversationService::purgeExpired(int64_t now, int64_t retentionDays) const
{
  std::scoped_lock lock(mutex_);
  return repository_.purgeTurnsBefore(db_, now - retentionDays * 86400);
}
