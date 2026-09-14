#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

namespace person_tag_query
{
inline constexpr std::string_view FIND_BY_PERSON =
    "SELECT tag FROM person_tag WHERE person_id = ? "
    "ORDER BY created_at ASC, id ASC";

inline constexpr std::string_view INSERT =
    "INSERT OR IGNORE INTO person_tag (person_id, tag, source) VALUES (?, ?, ?)";
} // namespace person_tag_query

struct PersonTagAddInput
{
  int64_t personId{0};
  std::vector<std::string> tags;
  std::string source{"llm"};
};
