#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace person_query
{

inline constexpr std::string_view FIND_BY_ID =
    "SELECT * FROM person WHERE id = ? AND deleted_at IS NULL";

inline constexpr std::string_view FIND_BY_USER =
    "SELECT * FROM person WHERE user_id = ? AND deleted_at IS NULL";

inline constexpr std::string_view INSERT =
    "INSERT INTO person (user_id, name, alias, observation, "
    "first_seen_at, last_seen_at) VALUES (?, ?, ?, ?, "
    "strftime('%s','now'), strftime('%s','now'))";

inline constexpr std::string_view UPDATE =
    "UPDATE person SET name = ?, alias = ?, observation = ?, "
    "last_seen_at = strftime('%s', 'now'), "
    "updated_at = strftime('%s', 'now') "
    "WHERE id = ? AND deleted_at IS NULL";

inline constexpr std::string_view UPDATE_USER =
    "UPDATE person SET user_id = ?, "
    "updated_at = strftime('%s', 'now') "
    "WHERE id = ? AND deleted_at IS NULL";

inline constexpr std::string_view REMOVE =
    "UPDATE person SET deleted_at = strftime('%s', 'now'), "
    "updated_at = strftime('%s', 'now') WHERE id = ? AND deleted_at IS NULL";

} // namespace person_query

struct PersonCreateInput
{
  std::optional<int64_t> userId;
  std::string name;
  std::string alias;
  std::string observation;
};

struct PersonUpdateInput
{
  std::string name;
  std::string alias;
  std::string observation;
};
