#pragma once

#include <shared/repositories/memory-lexicon/memory-lexicon-query.hxx>
#include <shared/services/extract/extract-contracts.hxx>
#include <vector>

struct sqlite3;

class MemoryLexiconRepository
{
public:
  std::vector<extract::LexiconEntry> allEntries(sqlite3* db);
  bool addEntry(sqlite3* db, const LexiconWriteInput& input);
  bool removeEntry(sqlite3* db, const LexiconDeleteInput& input);
};
