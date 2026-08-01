#pragma once

#include <cstdint>
#include <optional>
#include <shared/enums.hxx>
#include <string>
#include <string_view>

namespace user_query
{

inline constexpr std::string_view FIND_BY_ID =
    "SELECT * FROM user WHERE id = ? AND deleted_at IS NULL";

inline constexpr std::string_view FIND_ALL =
    "SELECT * FROM user "
    "WHERE deleted_at IS NULL "
    "ORDER BY created_at ASC LIMIT 200";

inline constexpr std::string_view FIND =
    "SELECT * FROM user "
    "WHERE deleted_at IS NULL AND created_at >= ? AND created_at <= ? "
    "ORDER BY created_at ASC LIMIT 200";

inline constexpr std::string_view FIND_FROM =
    "SELECT * FROM user "
    "WHERE deleted_at IS NULL AND created_at >= ? "
    "ORDER BY created_at ASC LIMIT 200";

inline constexpr std::string_view FIND_DELETED =
    "SELECT * FROM user "
    "WHERE deleted_at IS NOT NULL AND deleted_at >= ? AND deleted_at <= ? "
    "ORDER BY deleted_at ASC LIMIT 200";

inline constexpr std::string_view FIND_DELETED_FROM =
    "SELECT * FROM user "
    "WHERE deleted_at IS NOT NULL AND deleted_at >= ? "
    "ORDER BY deleted_at ASC LIMIT 200";

inline constexpr std::string_view FIND_DELETED_ALL =
    "SELECT * FROM user "
    "WHERE deleted_at IS NOT NULL "
    "ORDER BY deleted_at ASC LIMIT 200";

inline constexpr std::string_view FIND_LAST =
    "SELECT * FROM user "
    "WHERE deleted_at IS NULL "
    "ORDER BY created_at DESC LIMIT 1";

inline constexpr std::string_view FIND_LAST_DELETED =
    "SELECT * FROM user "
    "WHERE deleted_at IS NOT NULL "
    "ORDER BY deleted_at DESC LIMIT 1";

inline constexpr std::string_view INSERT =
    "INSERT INTO user (name, last_name, role, is_active) "
    "VALUES (?, ?, ?, ?)";

inline constexpr std::string_view UPDATE_PREFIX = "UPDATE user SET ";
inline constexpr std::string_view UPDATE_COL_NAME = "name = ?";
inline constexpr std::string_view UPDATE_COL_LAST_NAME = "last_name = ?";
inline constexpr std::string_view UPDATE_COL_ROLE = "role = ?";
inline constexpr std::string_view UPDATE_COL_IS_ACTIVE = "is_active = ?";
inline constexpr std::string_view UPDATE_SUFFIX =
    ", updated_at = strftime('%s', 'now') "
    "WHERE id = ? AND deleted_at IS NULL";

inline constexpr std::string_view REMOVE =
    "UPDATE user SET deleted_at = strftime('%s', 'now'), "
    "updated_at = strftime('%s', 'now') "
    "WHERE id = ? AND deleted_at IS NULL";

} // namespace user_query

struct UserCreateInput
{
  std::string name;
  std::string lastName;
  UserRole role{UserRole::Guest};
};

struct UserUpdateInput
{
  std::optional<std::string> name;
  std::optional<std::string> lastName;
  std::optional<UserRole> role;
  std::optional<bool> isActive;
};
