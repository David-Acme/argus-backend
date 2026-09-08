#pragma once

#include <array>
#include <cstdint>
#include <shared/enums.hxx>
#include <span>
#include <string_view>

#include <shared/vocabulary/vocabulary-es.hxx>
#include <shared/vocabulary/vocabulary-en.hxx>

// Static per-language vocabulary; the entries live in the language headers.
// Domain-neutral on purpose: the extraction lexicon built from these seeds
// lives with the extractor that consumes it (argus-memory).
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

} // namespace vocabulary
