#pragma once

#include <shared/services/extract/extract-contracts.hxx>
#include <shared/vocabulary/vocabulary.hxx>

#include <string>
#include <vector>

namespace extract
{

// The static vocabulary seeds flattened into lexicon entries. It lives here
// rather than in the vocabulary headers so the phrase data stays free of the
// extraction contract.
inline std::vector<LexiconEntry> allLexiconEntries()
{
  std::vector<LexiconEntry> out;
  const auto es = vocabulary::spanishLexicon();
  const auto en = vocabulary::englishLexicon();
  out.reserve(es.size() + en.size());
  for (const auto& seed : es)
    out.push_back({.kind = seed.kind,
                   .lang = "es",
                   .surface = std::string(seed.surface),
                   .canonical = std::string(seed.canonical)});
  for (const auto& seed : en)
    out.push_back({.kind = seed.kind,
                   .lang = "en",
                   .surface = std::string(seed.surface),
                   .canonical = std::string(seed.canonical)});
  return out;
}

} // namespace extract
