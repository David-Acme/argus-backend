#pragma once

namespace memory_phrase_query
{

inline constexpr const char* FIND_PHRASES =
    "SELECT kind, lang, phrase, memory_type FROM memory_phrase "
    "ORDER BY length(phrase) DESC";

inline constexpr const char* INSERT_PHRASE =
    "INSERT OR IGNORE INTO memory_phrase (kind, lang, phrase, memory_type) "
    "VALUES (?, ?, ?, ?)";

inline constexpr const char* DELETE_PHRASE =
    "DELETE FROM memory_phrase WHERE kind = ? AND lang = ? AND phrase = ?";

} // namespace memory_phrase_query
