#include "relay-thread-repository.hxx"

#include <sqlite/sqlite-stmt.hxx>

#include <sqlite3.h>

#include <string>
#include <utility>

using namespace relay_thread_query;

namespace
{

std::optional<RelayThreadRow> rowOf(const SqliteStmt& stmt)
{
  const auto channel = relayChannelFromString(stmt.columnText(4));
  const auto state = relayThreadStateFromString(stmt.columnText(5));
  if (!channel || !state)
    return std::nullopt;
  return RelayThreadRow{.id = stmt.columnInt64(0),
                        .fromUserId = stmt.columnInt64(1),
                        .toUserId = stmt.columnInt64(2),
                        .messageId = stmt.columnText(3),
                        .channel = *channel,
                        .state = *state,
                        .replyUntil = stmt.columnInt64(6),
                        .createdAt = stmt.columnInt64(7)};
}

}

int64_t RelayThreadRepository::create(sqlite3* db, const RelayThreadCreateInput& input) const
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, INSERT_THREAD))
    return 0;
  stmt.bindInt64(1, input.fromUserId);
  stmt.bindInt64(2, input.toUserId);
  stmt.bindText(3, input.messageId);
  stmt.bindText(4, std::string(relayChannelToString(input.channel)));
  stmt.bindInt64(5, input.replyUntil);
  stmt.bindInt64(6, input.at);
  if (stmt.step() != SQLITE_DONE)
    return 0;
  return sqlite3_last_insert_rowid(db);
}

std::optional<RelayThreadRow> RelayThreadRepository::openFor(sqlite3* db, const RelayThreadOpenInput& input) const
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, (std::string(SELECT_COLUMNS) + WHERE_OPEN_FOR_RECIPIENT).c_str()))
    return std::nullopt;
  stmt.bindInt64(1, input.toUserId);
  stmt.bindInt64(2, input.now);
  if (stmt.step() != SQLITE_ROW)
    return std::nullopt;
  return rowOf(stmt);
}

int64_t RelayThreadRepository::countFrom(sqlite3* db, const RelayThreadCountInput& input) const
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, COUNT_FROM_SENDER_SINCE))
    return 0;
  stmt.bindInt64(1, input.fromUserId);
  stmt.bindInt64(2, input.since);
  if (stmt.step() != SQLITE_ROW)
    return 0;
  return stmt.columnInt64(0);
}

std::vector<RelayThreadRow> RelayThreadRepository::openThreads(sqlite3* db, const RelayThreadOpenInput& input) const
{
  std::vector<RelayThreadRow> rows;
  SqliteStmt stmt;
  if (!stmt.prepare(db, (std::string(SELECT_COLUMNS) + WHERE_OPEN_THREADS_FOR_RECIPIENT).c_str()))
    return rows;
  stmt.bindInt64(1, input.toUserId);
  stmt.bindInt64(2, input.now);
  while (stmt.step() == SQLITE_ROW)
    if (auto row = rowOf(stmt))
      rows.push_back(std::move(*row));
  return rows;
}

bool RelayThreadRepository::setDelivery(sqlite3* db, const RelayThreadDeliveryInput& input) const
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, SET_DELIVERY))
    return false;
  stmt.bindText(1, std::string(relayChannelToString(input.channel)));
  stmt.bindText(2, input.messageId);
  stmt.bindInt64(3, input.id);
  return stmt.step() == SQLITE_DONE && sqlite3_changes(db) == 1;
}

bool RelayThreadRepository::remove(sqlite3* db, int64_t id) const
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, REMOVE_THREAD))
    return false;
  stmt.bindInt64(1, id);
  return stmt.step() == SQLITE_DONE && sqlite3_changes(db) == 1;
}

bool RelayThreadRepository::answer(sqlite3* db, int64_t id) const
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, ANSWER_THREAD))
    return false;
  stmt.bindInt64(1, id);
  return stmt.step() == SQLITE_DONE && sqlite3_changes(db) == 1;
}

std::vector<std::string> RelayThreadRepository::expire(sqlite3* db, int64_t now) const
{
  std::vector<std::string> expired;
  SqliteStmt stmt;
  if (!stmt.prepare(db, EXPIRE_THREADS))
    return expired;
  stmt.bindInt64(1, now);
  while (stmt.step() == SQLITE_ROW)
    expired.push_back(stmt.columnText(0));
  return expired;
}

int RelayThreadRepository::purge(sqlite3* db, int64_t before) const
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, PURGE_SETTLED))
    return 0;
  stmt.bindInt64(1, before);
  if (stmt.step() != SQLITE_DONE)
    return 0;
  return sqlite3_changes(db);
}
