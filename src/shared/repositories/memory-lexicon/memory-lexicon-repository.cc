#include "memory-lexicon-repository.hxx"

#include <shared/repositories/memory-lexicon/memory-lexicon-query.hxx>
#include <shared/wrapper/sqlite-stmt/sqlite-stmt.hxx>
#include <sqlite3.h>

using namespace memory_lexicon_query;

std::vector<extract::LexiconEntry>
MemoryLexiconRepository::allEntries(sqlite3* db)
{
  std::vector<extract::LexiconEntry> out;
  if (!db)
    return out;
  SqliteStmt stmt;
  if (!stmt.prepare(db, FIND_LEXICON))
    return out;
  while (stmt.step() == SQLITE_ROW) {
    out.push_back({.kind = lexiconKindFromString(stmt.columnText(0)),
                   .lang = stmt.columnText(1),
                   .surface = stmt.columnText(2),
                   .canonical = stmt.columnText(3)});
  }
  return out;
}

bool MemoryLexiconRepository::addEntry(sqlite3* db,
                                       const LexiconWriteInput& input)
{
  if (!db || input.surface.empty())
    return false;
  SqliteStmt stmt;
  if (!stmt.prepare(db, INSERT_LEXICON))
    return false;
  stmt.bindText(1, lexiconKindToString(input.kind));
  stmt.bindText(2, input.lang.empty() ? "es" : input.lang);
  stmt.bindText(3, input.surface);
  stmt.bindText(4, input.canonical);
  return stmt.step() == SQLITE_DONE;
}

bool MemoryLexiconRepository::removeEntry(sqlite3* db, LexiconKind kind,
                                          const std::string& lang,
                                          const std::string& surface)
{
  if (!db || surface.empty())
    return false;
  SqliteStmt stmt;
  if (!stmt.prepare(db, DELETE_LEXICON))
    return false;
  stmt.bindText(1, lexiconKindToString(kind));
  stmt.bindText(2, lang.empty() ? "es" : lang);
  stmt.bindText(3, surface);
  return stmt.step() == SQLITE_DONE;
}
