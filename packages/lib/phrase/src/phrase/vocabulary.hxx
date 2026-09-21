#pragma once

#include <array>
#include <cstdint>
#include <span>
#include <string_view>

#include <phrase/details/vocabulary-en.hxx>
#include <phrase/details/vocabulary-es.hxx>

// Static per-language vocabulary; entries live in the language headers.
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
