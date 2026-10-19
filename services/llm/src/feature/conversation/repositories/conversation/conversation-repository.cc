#include "conversation-repository.hxx"

#include <sqlite/sqlite-stmt.hxx>

#include <sqlite3.h>

#include <utility>

using namespace conversation_query;

namespace
{
std::optional<ConversationRow> conversationOf(const SqliteStmt& stmt)
{
  const auto leg = conversationLegKindFromString(stmt.columnText(1));
  if (!leg)
    return std::nullopt;
  return ConversationRow{.userId = stmt.columnInt64(0),
                         .activeLeg = *leg,
                         .summary = stmt.columnText(2),
                         .summaryTurns = stmt.columnInt64(3),
                         .lastTurnAt = stmt.columnInt64(4),
                         .createdAt = stmt.columnInt64(5),
                         .updatedAt = stmt.columnInt64(6)};
}

std::optional<ConversationTurnRow> turnOf(const SqliteStmt& stmt)
{
  const auto role = conversationTurnRoleFromString(stmt.columnText(2));
  const auto leg = conversationLegKindFromString(stmt.columnText(3));
  if (!role || !leg)
    return std::nullopt;
  return ConversationTurnRow{.id = stmt.columnInt64(0),
                             .userId = stmt.columnInt64(1),
                             .role = *role,
                             .leg = *leg,
                             .content = stmt.columnText(4),
                             .createdAt = stmt.columnInt64(5)};
}
}

bool ConversationRepository::touchLeg(sqlite3* db, const ConversationLegTouchInput& input) const
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, UPSERT_LEG))
    return false;
  stmt.bindInt64(1, input.userId);
  stmt.bindText(2, conversationLegKindToString(input.leg));
  stmt.bindInt64(3, input.at);
  return stmt.step() == SQLITE_DONE;
}

std::optional<ConversationRow> ConversationRepository::find(sqlite3* db, int64_t userId) const
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, SELECT_CONVERSATION))
    return std::nullopt;
  stmt.bindInt64(1, userId);
  if (stmt.step() != SQLITE_ROW)
    return std::nullopt;
  return conversationOf(stmt);
}

bool ConversationRepository::appendTurn(sqlite3* db, const ConversationTurnAppendInput& input) const
{
  SqliteStmt insert;
  if (!insert.prepare(db, INSERT_TURN))
    return false;
  insert.bindInt64(1, input.userId);
  insert.bindText(2, conversationTurnRoleToString(input.role));
  insert.bindText(3, conversationLegKindToString(input.leg));
  insert.bindText(4, input.content);
  insert.bindInt64(5, input.at);
  if (insert.step() != SQLITE_DONE)
    return false;
  SqliteStmt touch;
  if (!touch.prepare(db, TOUCH_LAST_TURN))
    return false;
  touch.bindInt64(1, input.at);
  touch.bindInt64(2, input.at);
  touch.bindInt64(3, input.userId);
  return touch.step() == SQLITE_DONE;
}

std::vector<ConversationTurnRow> ConversationRepository::recentTurns(
    sqlite3* db, const ConversationRecentTurnsInput& input) const
{
  std::vector<ConversationTurnRow> rows;
  SqliteStmt stmt;
  if (!stmt.prepare(db, SELECT_RECENT_TURNS))
    return rows;
  stmt.bindInt64(1, input.userId);
  stmt.bindInt64(2, input.limit);
  while (stmt.step() == SQLITE_ROW) {
    if (auto turn = turnOf(stmt))
      rows.push_back(std::move(*turn));
  }
  return rows;
}

bool ConversationRepository::setSummary(sqlite3* db, const ConversationSummaryInput& input) const
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, UPDATE_SUMMARY))
    return false;
  stmt.bindText(1, input.summary);
  stmt.bindInt64(2, input.summaryTurns);
  stmt.bindInt64(3, input.at);
  stmt.bindInt64(4, input.userId);
  return stmt.step() == SQLITE_DONE;
}

std::vector<int64_t> ConversationRepository::idleBefore(sqlite3* db, int64_t before) const
{
  std::vector<int64_t> users;
  SqliteStmt stmt;
  if (!stmt.prepare(db, SELECT_IDLE_USERS))
    return users;
  stmt.bindInt64(1, before);
  while (stmt.step() == SQLITE_ROW)
    users.push_back(stmt.columnInt64(0));
  return users;
}

int ConversationRepository::purgeTurnsBefore(sqlite3* db, int64_t before) const
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, PURGE_TURNS_BEFORE))
    return 0;
  stmt.bindInt64(1, before);
  if (stmt.step() != SQLITE_DONE)
    return 0;
  return sqlite3_changes(db);
}
