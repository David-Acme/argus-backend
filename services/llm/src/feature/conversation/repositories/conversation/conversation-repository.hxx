#pragma once

#include "conversation-query.hxx"

#include <cstdint>
#include <optional>
#include <vector>

struct sqlite3;

class ConversationRepository
{
public:
  [[nodiscard]] bool touchLeg(sqlite3* db, const ConversationLegTouchInput& input) const;
  [[nodiscard]] std::optional<ConversationRow> find(sqlite3* db, int64_t userId) const;
  [[nodiscard]] bool appendTurn(sqlite3* db, const ConversationTurnAppendInput& input) const;
  [[nodiscard]] std::vector<ConversationTurnRow> recentTurns(sqlite3* db,
                                                             const ConversationRecentTurnsInput& input) const;
  [[nodiscard]] bool setSummary(sqlite3* db, const ConversationSummaryInput& input) const;
  [[nodiscard]] int purgeTurnsBefore(sqlite3* db, int64_t before) const;
  [[nodiscard]] std::vector<int64_t> idleBefore(sqlite3* db, int64_t before) const;
};
