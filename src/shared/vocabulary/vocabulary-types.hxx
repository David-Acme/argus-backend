#pragma once

#include <shared/enums.hxx>
#include <string_view>

// Static per-language vocabulary entries for the memory rule engine
// (src/shared/vocabulary/). Single source of truth: adding an entry means
// editing the language header, never a DB table.
struct PhraseSeed
{
  PhraseKind kind;
  MemoryType memoryType;
  std::string_view phrase;
};

struct LexiconSeed
{
  LexiconKind kind;
  std::string_view surface;
  std::string_view canonical;
};
