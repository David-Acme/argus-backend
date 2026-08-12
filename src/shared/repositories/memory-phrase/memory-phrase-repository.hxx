#pragma once

#include <shared/enums.hxx>
#include <string>
#include <vector>

struct sqlite3;

struct PhraseRow
{
  PhraseKind kind;
  std::string lang;
  std::string phrase;
  MemoryType memoryType;
};

struct PhraseWriteInput
{
  PhraseKind kind;
  std::string lang;
  std::string phrase;
  MemoryType memoryType;
};

class MemoryPhraseRepository
{
public:
  std::vector<PhraseRow> allPhrases(sqlite3* db);
  bool addPhrase(sqlite3* db, const PhraseWriteInput& input);
  bool removePhrase(sqlite3* db, PhraseKind kind, const std::string& lang,
                    const std::string& phrase);
};
