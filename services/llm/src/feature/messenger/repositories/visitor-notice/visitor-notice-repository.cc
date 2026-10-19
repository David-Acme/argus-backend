#include "visitor-notice-repository.hxx"

#include <sqlite/sqlite-stmt.hxx>

#include <sqlite3.h>

using namespace visitor_notice_query;

int64_t VisitorNoticeRepository::record(sqlite3* db, const VisitorNoticeWriteInput& input) const
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, INSERT_NOTICE))
    return 0;
  stmt.bindText(1, input.camera);
  stmt.bindInt64(2, input.toUserId);
  stmt.bindInt64(3, input.at);
  if (stmt.step() != SQLITE_DONE)
    return 0;
  return sqlite3_last_insert_rowid(db);
}

bool VisitorNoticeRepository::remove(sqlite3* db, int64_t id) const
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, REMOVE_NOTICE))
    return false;
  stmt.bindInt64(1, id);
  return stmt.step() == SQLITE_DONE && sqlite3_changes(db) == 1;
}

int VisitorNoticeRepository::purge(sqlite3* db, int64_t before) const
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, PURGE_BEFORE))
    return 0;
  stmt.bindInt64(1, before);
  if (stmt.step() != SQLITE_DONE)
    return 0;
  return sqlite3_changes(db);
}

int64_t VisitorNoticeRepository::countSince(sqlite3* db, const VisitorNoticeCountInput& input) const
{
  SqliteStmt stmt;
  if (!stmt.prepare(db, COUNT_SINCE))
    return 0;
  stmt.bindText(1, input.camera);
  stmt.bindInt64(2, input.toUserId);
  stmt.bindInt64(3, input.since);
  if (stmt.step() != SQLITE_ROW)
    return 0;
  return stmt.columnInt64(0);
}
