#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace project_query
{
/** `?` the ownership predicate spends in every sync query of this table. */
inline constexpr int OWNERSHIP_PLACEHOLDERS = 2;


inline constexpr std::string_view FIND_BY_ID =
    "SELECT * FROM project WHERE id = ? AND deleted_at IS NULL";

inline constexpr std::string_view FIND =
    "SELECT * FROM project "
    "WHERE deleted_at IS NULL AND created_at >= ? AND created_at <= ? "
    "AND (owner_id = ? OR EXISTS (SELECT 1 FROM project_member m "
    "WHERE m.project_id = project.id AND m.user_id = ? "
    "AND m.deleted_at IS NULL)) "
    "ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_FROM =
    "SELECT * FROM project "
    "WHERE deleted_at IS NULL AND created_at >= ? "
    "AND (owner_id = ? OR EXISTS (SELECT 1 FROM project_member m "
    "WHERE m.project_id = project.id AND m.user_id = ? "
    "AND m.deleted_at IS NULL)) "
    "ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_AFTER =
    "SELECT * FROM project WHERE deleted_at IS NULL AND "
    "(created_at > ? OR (created_at = ? AND id > ?)) AND created_at <= ? "
    "AND (owner_id = ? OR EXISTS (SELECT 1 FROM project_member m "
    "WHERE m.project_id = project.id AND m.user_id = ? "
    "AND m.deleted_at IS NULL)) "
    "ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_AFTER_FROM =
    "SELECT * FROM project WHERE deleted_at IS NULL AND "
    "(created_at > ? OR (created_at = ? AND id > ?)) "
    "AND (owner_id = ? OR EXISTS (SELECT 1 FROM project_member m "
    "WHERE m.project_id = project.id AND m.user_id = ? "
    "AND m.deleted_at IS NULL)) "
    "ORDER BY created_at ASC, id ASC LIMIT 200";

inline constexpr std::string_view FIND_DELETED =
    "SELECT * FROM project "
    "WHERE deleted_at IS NOT NULL AND deleted_at >= ? AND deleted_at <= ? "
    "AND (owner_id = ? OR EXISTS (SELECT 1 FROM project_member m "
    "WHERE m.project_id = project.id AND m.user_id = ? "
    "AND m.deleted_at IS NULL)) "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_FROM =
    "SELECT * FROM project "
    "WHERE deleted_at IS NOT NULL AND deleted_at >= ? "
    "AND (owner_id = ? OR EXISTS (SELECT 1 FROM project_member m "
    "WHERE m.project_id = project.id AND m.user_id = ? "
    "AND m.deleted_at IS NULL)) "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_AFTER =
    "SELECT * FROM project WHERE deleted_at IS NOT NULL AND "
    "(deleted_at > ? OR (deleted_at = ? AND id > ?)) AND deleted_at <= ? "
    "AND (owner_id = ? OR EXISTS (SELECT 1 FROM project_member m "
    "WHERE m.project_id = project.id AND m.user_id = ? "
    "AND m.deleted_at IS NULL)) "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_AFTER_FROM =
    "SELECT * FROM project WHERE deleted_at IS NOT NULL AND "
    "(deleted_at > ? OR (deleted_at = ? AND id > ?)) "
    "AND (owner_id = ? OR EXISTS (SELECT 1 FROM project_member m "
    "WHERE m.project_id = project.id AND m.user_id = ? "
    "AND m.deleted_at IS NULL)) "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";

inline constexpr std::string_view FIND_ALL =
    "SELECT * FROM project WHERE deleted_at IS NULL "
    "AND (owner_id = ? OR EXISTS (SELECT 1 FROM project_member m "
    "WHERE m.project_id = project.id AND m.user_id = ? "
    "AND m.deleted_at IS NULL)) "
    "ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_ALL =
    "SELECT * FROM project WHERE deleted_at IS NOT NULL "
    "AND (owner_id = ? OR EXISTS (SELECT 1 FROM project_member m "
    "WHERE m.project_id = project.id AND m.user_id = ? "
    "AND m.deleted_at IS NULL)) "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";

inline constexpr std::string_view FIND_LAST =
    "SELECT * FROM project WHERE deleted_at IS NULL "
    "AND (owner_id = ? OR EXISTS (SELECT 1 FROM project_member m "
    "WHERE m.project_id = project.id AND m.user_id = ? "
    "AND m.deleted_at IS NULL)) "
    "ORDER BY created_at DESC LIMIT 1";
inline constexpr std::string_view FIND_LAST_DELETED =
    "SELECT * FROM project WHERE deleted_at IS NOT NULL "
    "AND (owner_id = ? OR EXISTS (SELECT 1 FROM project_member m "
    "WHERE m.project_id = project.id AND m.user_id = ? "
    "AND m.deleted_at IS NULL)) "
    "ORDER BY deleted_at DESC LIMIT 1";

inline constexpr std::string_view FIND_BY_OWNER =
    "SELECT * FROM project WHERE owner_id = ? AND deleted_at IS NULL "
    "ORDER BY created_at DESC";

inline constexpr std::string_view INSERT =
    "INSERT INTO project (owner_id, name, description, status, color, "
    "starts_at, target_at) VALUES (?, ?, ?, ?, ?, ?, ?)";

inline constexpr std::string_view UPDATE_PREFIX = "UPDATE project SET ";
inline constexpr std::string_view UPDATE_COL_NAME = "name = ?";
inline constexpr std::string_view UPDATE_COL_DESCRIPTION = "description = ?";
inline constexpr std::string_view UPDATE_COL_STATUS = "status = ?";
inline constexpr std::string_view UPDATE_COL_COLOR = "color = ?";
inline constexpr std::string_view UPDATE_COL_STARTS_AT = "starts_at = ?";
inline constexpr std::string_view UPDATE_COL_TARGET_AT = "target_at = ?";
inline constexpr std::string_view UPDATE_SUFFIX =
    ", updated_at = strftime('%s', 'now') "
    "WHERE id = ? AND deleted_at IS NULL";

inline constexpr std::string_view REMOVE =
    "UPDATE project SET deleted_at = strftime('%s', 'now'), "
    "updated_at = strftime('%s', 'now') "
    "WHERE id = ? AND deleted_at IS NULL";

} // namespace project_query

struct ProjectCreateInput
{
  int64_t ownerId{0};
  std::string name;
  std::string description;
  std::string status;
  std::string color;
  std::optional<int64_t> startsAt;
  std::optional<int64_t> targetAt;
};

struct ProjectUpdateInput
{
  std::optional<std::string> name;
  std::optional<std::string> description;
  std::optional<std::string> status;
  std::optional<std::string> color;
  std::optional<int64_t> startsAt;
  std::optional<int64_t> targetAt;
};
