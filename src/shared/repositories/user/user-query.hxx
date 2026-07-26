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

inline constexpr std::string_view FIND_ALL =
    "SELECT * FROM user "
    "WHERE deleted_at IS NULL "
    "ORDER BY created_at ASC LIMIT 200";

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
    "INSERT INTO user (feature_hub_id, name, last_name, role, is_active) "
    "VALUES (?, ?, ?, ?, ?)";

inline constexpr std::string_view UPDATE =
    "UPDATE user SET name = ?, last_name = ?, role = ?, is_active = ?, "
    "updated_at = strftime('%s', 'now') "
    "WHERE id = ? AND deleted_at IS NULL";

inline constexpr std::string_view REMOVE =
    "UPDATE user SET deleted_at = strftime('%s', 'now'), "
    "updated_at = strftime('%s', 'now') "
    "WHERE id = ? AND deleted_at IS NULL";

} // namespace user_query

struct UserCreateInput
{
  int64_t featureHubId{0};
  std::string name;
  std::string lastName;
  UserRole role{UserRole::Guest};
};

struct UserUpdateInput
{
  std::string name;
  std::string lastName;
  UserRole role{UserRole::Guest};
  bool isActive{true};
};

