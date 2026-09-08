#pragma once

#include <string>
#include <unordered_set>
#include <vector>

// Single home for text normalisation primitives.
namespace text_norm
{

// UTF-8-aware word splitting: accented words stay whole; lowercased, minAlnum letters.
std::vector<std::string> words(const std::string& text, int minAlnum = 3);

// Same splitter as words(), returned as a set (for set-based similarity).
std::unordered_set<std::string> wordSet(const std::string& text,
                                        int minAlnum = 4);

// Collapses whitespace runs into single spaces; leading runs dropped.
std::string whitespace(const std::string& text, bool toLower = true);

// Fold Spanish diacritics to ASCII: áéíóúüñ -> aeiouun (and uppercase forms).
std::string stripAccents(std::string text);

// Intent pre-normalisation: strips Spanish inverted marks, maps punctuation to spaces; case-preserving.
std::string intent(const std::string& text);

} // namespace text_norm
