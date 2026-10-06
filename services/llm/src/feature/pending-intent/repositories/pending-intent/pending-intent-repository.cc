#include "pending-intent-repository.hxx"

#include <sqlite/sqlite-stmt.hxx>

#include <sqlite3.h>

using namespace pending_intent_query;

namespace
{
PendingIntentRow rowOf(const SqliteStmt& stmt)
{
  return {.id = stmt.columnInt64(0),
          .userId = stmt.columnInt64(1),
          .role = stmt.columnText(2),
          .module = stmt.columnText(3),
          .tool = stmt.columnText(4),
          .arguments = stmt.columnText(5),
          .lang = stmt.columnText(6),
          .utterance = stmt.columnText(7),
          .sessionId = stmt.columnText(8),
          .state = pendingIntentStateFromString(stmt.columnText(9)).value_or(PendingIntentState::Expired),
          .detail = stmt.columnText(10),
          .createdAt = stmt.columnInt64(11),
          .updatedAt = stmt.columnInt64(12)};
}

std::vector<PendingIntentRow> select(sqlite3* db, const std::string& where, const std::optional<std::string>& text,
                                     const std::optional<int64_t>& number)
{
  std::vector<PendingIntentRow> rows;
  SqliteStmt stmt;
  if (!stmt.prepare(db, (std::string(SELECT_COLUMNS) + where).c_str()))
    return rows;
  if (text)
    stmt.bindText(1, *text);
  if (number)
    stmt.bindInt64(1, *number);
  while (stmt.step() == SQLITE_ROW)
    rows.push_back(rowOf(stmt));
  return rows;
}
}

int64_t PendingIntentRepository::offer(sqlite3* db, const PendingIntentCreateInput& input) const
{
  SqliteStmt retire;
  if (retire.prepare(db, RETIRE_OFFERS)) {
    retire.bindInt64(1, input.at);
    retire.bindInt64(2, input.userId);
    retire.bindText(3, input.module);
    retire.step();
  }
  SqliteStmt stmt;
  if (!stmt.prepare(db, INSERT_OFFER))
    return 0;
  stmt.bindInt64(1, input.userId);
  stmt.bindText(2, input.role);
  stmt.bindText(3, input.module);
  stmt.bindText(4, input.tool);
  stmt.bindText(5, input.arguments);
  stmt.bindText(6, input.lang);
  stmt.bindText(7, input.utterance);
  stmt.bindText(8, input.sessionId);
  stmt.bindInt64(9, input.at);
  stmt.bindInt64(10, input.at);
  return stmt.step() == SQLITE_DONE ? sqlite3_last_insert_rowid(db) : 0;
}

std::vector<int64_t> PendingIntentRepository::accept(sqlite3* db, const PendingIntentAcceptInput& input) const
{
  std::vector<int64_t> ids;
  SqliteStmt stmt;
  if (!stmt.prepare(db, ACCEPT_OFFERS))
    return ids;
  stmt.bindInt64(1, input.at);
  stmt.bindInt64(2, input.userId);
  stmt.bindText(3, input.module);
  while (stmt.step() == SQLITE_ROW)
    ids.push_back(stmt.columnInt64(0));
  return ids;
}

std::optional<PendingIntentRow> PendingIntentRepository::find(sqlite3* db, int64_t id) const
{
  auto rows = select(db, WHERE_ID, std::nullopt, id);
  if (rows.empty())
    return std::nullopt;
  return std::move(rows.front());
}

std::vector<PendingIntentRow> PendingIntentRepository::waiting(sqlite3* db, const std::string& module) const
{
  if (module.empty())
    return select(db, WHERE_WAITING, std::nullopt, std::nullopt);
  return select(db, WHERE_WAITING_FOR_MODULE, module, std::nullopt);
}

std::vector<PendingIntentRow> PendingIntentRepository::waitingBefore(sqlite3* db, int64_t before) const
{
  return select(db, WHERE_WAITING_BEFORE, std::nullopt, before);
}

bool PendingIntentRepository::settle(sqlite3* db, const PendingIntentSettleInput& input) const
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, SETTLE))
    return false;
  stmt.bindText(1, std::string(pendingIntentStateToString(input.state)));
  stmt.bindText(2, input.detail);
  stmt.bindInt64(3, input.at);
  stmt.bindInt64(4, input.id);
  return stmt.step() == SQLITE_DONE && sqlite3_changes(db) > 0;
}

int PendingIntentRepository::expireOffers(sqlite3* db, int64_t before, int64_t at) const
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, EXPIRE_OFFERS))
    return 0;
  stmt.bindInt64(1, at);
  stmt.bindInt64(2, before);
  return stmt.step() == SQLITE_DONE ? sqlite3_changes(db) : 0;
}

int PendingIntentRepository::purge(sqlite3* db, int64_t before) const
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, PURGE_SETTLED))
    return 0;
  stmt.bindInt64(1, before);
  return stmt.step() == SQLITE_DONE ? sqlite3_changes(db) : 0;
}
