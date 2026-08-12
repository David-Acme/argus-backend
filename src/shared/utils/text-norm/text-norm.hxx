#pragma once

#include <string>
#include <unordered_set>
#include <vector>

// Single home for text normalisation primitives. Replaces the five
// near-duplicate tokenizers that used to live in memory-recall.cc,
// memory-store.cc, intent-service.cc and labs/voice-test.
namespace text_norm
{

// UTF-8-aware word splitting: bytes >= 0x80 stay glued to the current word so
// accented words ("café", "¿cuándo") are never split. Words are lowercased and
// only those with >= minAlnum real alphanumeric characters are kept.
std::vector<std::string> words(const std::string& text, int minAlnum = 3);

// Same splitter as words(), returned as a set (for set-based similarity).
std::unordered_set<std::string> wordSet(const std::string& text,
                                        int minAlnum = 4);

// Lowercase (unless toLower is false) and collapse whitespace runs into single
// spaces. Leading runs are dropped, a trailing space is kept.
std::string whitespace(const std::string& text, bool toLower = true);

// Fold Spanish diacritics to ASCII: áéíóúüñ -> aeiouun (and uppercase forms).
std::string stripAccents(std::string text);

// Intent pre-normalisation: strips Spanish inverted marks (¿¡), maps
// ?!.,;: to spaces, collapses whitespace runs, trims the trailing space.
// Case-preserving — the fastText model sees the original casing.
std::string intent(const std::string& text);

} // namespace text_norm
