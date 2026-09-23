#pragma once

#include <phrase/lexicon-kind.hxx>
#include <phrase/memory-type.hxx>
#include <phrase/details/phrase-kind.hxx>
#include <string_view>

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
