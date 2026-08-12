#pragma once

namespace memory_lexicon_query
{

inline constexpr const char* FIND_LEXICON =
    "SELECT kind, lang, surface, canonical FROM memory_lexicon "
    "ORDER BY length(surface) DESC";

inline constexpr const char* INSERT_LEXICON =
    "INSERT OR IGNORE INTO memory_lexicon (kind, lang, surface, canonical) "
    "VALUES (?, ?, ?, ?)";

inline constexpr const char* DELETE_LEXICON =
    "DELETE FROM memory_lexicon WHERE kind = ? AND lang = ? AND surface = ?";

} // namespace memory_lexicon_query
