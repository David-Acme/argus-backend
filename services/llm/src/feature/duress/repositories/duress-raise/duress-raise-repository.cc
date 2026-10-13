#include "duress-raise-repository.hxx"

#include <sqlite/sqlite-stmt.hxx>

#include <sqlite3.h>

using namespace duress_raise_query;

std::optional<int64_t> DuressRaiseRepository::add(sqlite3* db, const DuressRaiseCreateInput& input) const
{
  SqliteStmt insert;
  if (!insert.prepare(db, INSERT))
    return std::nullopt;
  insert.bindInt64(1, input.userId);
  insert.bindText(2, userRoleToString(input.role));
  insert.bindText(3, input.keyDigest);
  insert.bindInt64(4, input.saidAt);
  if (insert.step() != SQLITE_DONE)
    return std::nullopt;
  if (sqlite3_changes(db) > 0)
    return sqlite3_last_insert_rowid(db);
  SqliteStmt existing;
  if (!existing.prepare(db, SELECT_ID_OF_USER))
    return std::nullopt;
  existing.bindInt64(1, input.userId);
  if (existing.step() != SQLITE_ROW)
    return std::nullopt;
  return existing.columnInt64(0);
}

std::vector<DuressRaiseRow> DuressRaiseRepository::pending(sqlite3* db) const
{
  std::vector<DuressRaiseRow> rows;
  SqliteStmt select;
  if (!select.prepare(db, SELECT_ALL))
    return rows;
  while (select.step() == SQLITE_ROW)
    rows.push_back({.id = select.columnInt64(0),
                    .userId = select.columnInt64(1),
                    .role = userRoleFromString(select.columnText(2)),
                    .keyDigest = select.columnText(3),
                    .saidAt = select.columnInt64(4)});
  return rows;
}

bool DuressRaiseRepository::remove(sqlite3* db, int64_t id) const
{
  SqliteStmt erase;
  if (!erase.prepare(db, DELETE_BY_ID))
    return false;
  erase.bindInt64(1, id);
  return erase.step() == SQLITE_DONE;
}

bool DuressRaiseRepository::syncFully(sqlite3* db) const
{
  SqliteStmt pragma;
  if (!pragma.prepare(db, SYNCHRONOUS_FULL))
    return false;
  const int step = pragma.step();
  return step == SQLITE_DONE || step == SQLITE_ROW;
}

int DuressRaiseRepository::synchronous(sqlite3* db) const
{
  SqliteStmt pragma;
  if (!pragma.prepare(db, SYNCHRONOUS) || pragma.step() != SQLITE_ROW)
    return -1;
  return pragma.columnInt(0);
}
