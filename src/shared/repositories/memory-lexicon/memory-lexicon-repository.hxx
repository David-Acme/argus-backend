#pragma once

#include <shared/services/extract/extract-contracts.hxx>
#include <string>
#include <vector>

struct sqlite3;

struct LexiconWriteInput
{
  LexiconKind kind;
  std::string lang;
  std::string surface;
  std::string canonical;
};

class MemoryLexiconRepository
{
public:
  std::vector<extract::LexiconEntry> allEntries(sqlite3* db);
  bool addEntry(sqlite3* db, const LexiconWriteInput& input);
  bool removeEntry(sqlite3* db, LexiconKind kind, const std::string& lang,
                   const std::string& surface);
};
