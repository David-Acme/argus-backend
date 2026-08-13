#pragma once

#include <array>
#include <cstdint>
#include <shared/enums.hxx>
#include <shared/services/extract/extract-contracts.hxx>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <shared/vocabulary/vocabulary-es.hxx>
#include <shared/vocabulary/vocabulary-en.hxx>

// Static per-language vocabulary for the memory rule engine. Single source
// of truth: adding a phrase/lexicon entry = editing the language header.
// No DB tables, no runtime queries (was memory_phrase / memory_lexicon).
// PhraseSeed/LexiconSeed live in vocabulary-types.hxx.
namespace vocabulary
{

inline std::span<const PhraseSeed> spanishPhrases()
{
  return kESPhrases;
}

inline std::span<const PhraseSeed> englishPhrases()
{
  return kENPhrases;
}

inline std::span<const LexiconSeed> spanishLexicon()
{
  return kESLexicon;
}

inline std::span<const LexiconSeed> englishLexicon()
{
  return kENLexicon;
}

inline std::vector<extract::LexiconEntry> allLexiconEntries()
{
  std::vector<extract::LexiconEntry> out;
  out.reserve(kESLexicon.size() + kENLexicon.size());
  for (const auto& seed : kESLexicon)
    out.push_back({.kind = seed.kind,
                    .lang = "es",
                    .surface = std::string(seed.surface),
                    .canonical = std::string(seed.canonical)});
  for (const auto& seed : kENLexicon)
    out.push_back({.kind = seed.kind,
                    .lang = "en",
                    .surface = std::string(seed.surface),
                    .canonical = std::string(seed.canonical)});
  return out;
}

} // namespace vocabulary
