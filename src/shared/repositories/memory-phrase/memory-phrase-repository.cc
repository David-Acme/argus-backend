#include "memory-phrase-repository.hxx"

#include <shared/repositories/memory-phrase/memory-phrase-query.hxx>
#include <shared/wrapper/sqlite-stmt/sqlite-stmt.hxx>
#include <sqlite3.h>

using namespace memory_phrase_query;

std::vector<PhraseRow> MemoryPhraseRepository::allPhrases(sqlite3* db)
{
  std::vector<PhraseRow> out;
  if (!db)
    return out;
  SqliteStmt stmt;
  if (!stmt.prepare(db, FIND_PHRASES))
    return out;
  while (stmt.step() == SQLITE_ROW) {
    out.push_back({.kind = phraseKindFromString(stmt.columnText(0)),
                   .lang = stmt.columnText(1),
                   .phrase = stmt.columnText(2),
                   .memoryType = memoryTypeFromString(stmt.columnText(3))});
  }
  return out;
}

bool MemoryPhraseRepository::addPhrase(sqlite3* db,
                                       const PhraseWriteInput& input)
{
  if (!db || input.phrase.empty())
    return false;
  SqliteStmt stmt;
  if (!stmt.prepare(db, INSERT_PHRASE))
    return false;
  stmt.bindText(1, phraseKindToString(input.kind));
  stmt.bindText(2, input.lang.empty() ? "es" : input.lang);
  stmt.bindText(3, input.phrase);
  stmt.bindText(4, memoryTypeToString(input.memoryType));
  return stmt.step() == SQLITE_DONE;
}

bool MemoryPhraseRepository::removePhrase(sqlite3* db, PhraseKind kind,
                                          const std::string& lang,
                                          const std::string& phrase)
{
  if (!db || phrase.empty())
    return false;
  SqliteStmt stmt;
  if (!stmt.prepare(db, DELETE_PHRASE))
    return false;
  stmt.bindText(1, phraseKindToString(kind));
  stmt.bindText(2, lang.empty() ? "es" : lang);
  stmt.bindText(3, phrase);
  return stmt.step() == SQLITE_DONE;
}
