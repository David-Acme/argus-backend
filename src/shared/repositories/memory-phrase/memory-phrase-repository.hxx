#pragma once

#include <shared/repositories/memory-phrase/memory-phrase-query.hxx>
#include <string>
#include <vector>

struct sqlite3;

class MemoryPhraseRepository
{
public:
  std::vector<PhraseRow> allPhrases(sqlite3* db);
  bool addPhrase(sqlite3* db, const PhraseWriteInput& input);
  bool removePhrase(sqlite3* db, const PhraseDeleteInput& input);
};
