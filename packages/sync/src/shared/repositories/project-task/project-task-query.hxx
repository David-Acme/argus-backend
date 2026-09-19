#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace project_task_query
{
/** `?` the ownership predicate spends in every sync query of this table. */
inline constexpr int OWNERSHIP_PLACEHOLDERS = 2;


inline constexpr std::string_view FIND_BY_ID =
    "SELECT * FROM project_task WHERE id = ? AND deleted_at IS NULL";

inline constexpr std::string_view FIND =
    "SELECT * FROM project_task "
    "WHERE deleted_at IS NULL AND created_at >= ? AND created_at <= ? "
    "AND EXISTS (SELECT 1 FROM project p WHERE p.id = project_task.project_id "
    "AND (p.owner_id = ? OR EXISTS ("
    "SELECT 1 FROM project_member m WHERE m.project_id = p.id "
    "AND m.user_id = ? AND m.deleted_at IS NULL))) "
    "ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_FROM =
    "SELECT * FROM project_task "
    "WHERE deleted_at IS NULL AND created_at >= ? "
    "AND EXISTS (SELECT 1 FROM project p WHERE p.id = project_task.project_id "
    "AND (p.owner_id = ? OR EXISTS ("
    "SELECT 1 FROM project_member m WHERE m.project_id = p.id "
    "AND m.user_id = ? AND m.deleted_at IS NULL))) "
    "ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_AFTER =
    "SELECT * FROM project_task WHERE deleted_at IS NULL AND "
    "(created_at > ? OR (created_at = ? AND id > ?)) AND created_at <= ? "
    "AND EXISTS (SELECT 1 FROM project p WHERE p.id = project_task.project_id "
    "AND (p.owner_id = ? OR EXISTS ("
    "SELECT 1 FROM project_member m WHERE m.project_id = p.id "
    "AND m.user_id = ? AND m.deleted_at IS NULL))) "
    "ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_AFTER_FROM =
    "SELECT * FROM project_task WHERE deleted_at IS NULL AND "
    "(created_at > ? OR (created_at = ? AND id > ?)) "
    "AND EXISTS (SELECT 1 FROM project p WHERE p.id = project_task.project_id "
    "AND (p.owner_id = ? OR EXISTS ("
    "SELECT 1 FROM project_member m WHERE m.project_id = p.id "
    "AND m.user_id = ? AND m.deleted_at IS NULL))) "
    "ORDER BY created_at ASC, id ASC LIMIT 200";

inline constexpr std::string_view FIND_DELETED =
    "SELECT * FROM project_task "
    "WHERE deleted_at IS NOT NULL AND deleted_at >= ? AND deleted_at <= ? "
    "AND EXISTS (SELECT 1 FROM project p WHERE p.id = project_task.project_id "
    "AND (p.owner_id = ? OR EXISTS ("
    "SELECT 1 FROM project_member m WHERE m.project_id = p.id "
    "AND m.user_id = ? AND m.deleted_at IS NULL))) "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_FROM =
    "SELECT * FROM project_task "
    "WHERE deleted_at IS NOT NULL AND deleted_at >= ? "
    "AND EXISTS (SELECT 1 FROM project p WHERE p.id = project_task.project_id "
    "AND (p.owner_id = ? OR EXISTS ("
    "SELECT 1 FROM project_member m WHERE m.project_id = p.id "
    "AND m.user_id = ? AND m.deleted_at IS NULL))) "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_AFTER =
    "SELECT * FROM project_task WHERE deleted_at IS NOT NULL AND "
    "(deleted_at > ? OR (deleted_at = ? AND id > ?)) AND deleted_at <= ? "
    "AND EXISTS (SELECT 1 FROM project p WHERE p.id = project_task.project_id "
    "AND (p.owner_id = ? OR EXISTS ("
    "SELECT 1 FROM project_member m WHERE m.project_id = p.id "
    "AND m.user_id = ? AND m.deleted_at IS NULL))) "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_AFTER_FROM =
    "SELECT * FROM project_task WHERE deleted_at IS NOT NULL AND "
    "(deleted_at > ? OR (deleted_at = ? AND id > ?)) "
    "AND EXISTS (SELECT 1 FROM project p WHERE p.id = project_task.project_id "
    "AND (p.owner_id = ? OR EXISTS ("
    "SELECT 1 FROM project_member m WHERE m.project_id = p.id "
    "AND m.user_id = ? AND m.deleted_at IS NULL))) "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";

inline constexpr std::string_view FIND_ALL =
    "SELECT * FROM project_task WHERE deleted_at IS NULL "
    "AND EXISTS (SELECT 1 FROM project p WHERE p.id = project_task.project_id "
    "AND (p.owner_id = ? OR EXISTS ("
    "SELECT 1 FROM project_member m WHERE m.project_id = p.id "
    "AND m.user_id = ? AND m.deleted_at IS NULL))) "
    "ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_ALL =
    "SELECT * FROM project_task WHERE deleted_at IS NOT NULL "
    "AND EXISTS (SELECT 1 FROM project p WHERE p.id = project_task.project_id "
    "AND (p.owner_id = ? OR EXISTS ("
    "SELECT 1 FROM project_member m WHERE m.project_id = p.id "
    "AND m.user_id = ? AND m.deleted_at IS NULL))) "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";

inline constexpr std::string_view FIND_LAST =
    "SELECT * FROM project_task WHERE deleted_at IS NULL "
    "AND EXISTS (SELECT 1 FROM project p WHERE p.id = project_task.project_id "
    "AND (p.owner_id = ? OR EXISTS ("
    "SELECT 1 FROM project_member m WHERE m.project_id = p.id "
    "AND m.user_id = ? AND m.deleted_at IS NULL))) "
    "ORDER BY created_at DESC LIMIT 1";
inline constexpr std::string_view FIND_LAST_DELETED =
    "SELECT * FROM project_task WHERE deleted_at IS NOT NULL "
    "AND EXISTS (SELECT 1 FROM project p WHERE p.id = project_task.project_id "
    "AND (p.owner_id = ? OR EXISTS ("
    "SELECT 1 FROM project_member m WHERE m.project_id = p.id "
    "AND m.user_id = ? AND m.deleted_at IS NULL))) "
    "ORDER BY deleted_at DESC LIMIT 1";

inline constexpr std::string_view FIND_BY_PROJECT =
    "SELECT * FROM project_task WHERE project_id = ? AND deleted_at IS NULL "
    "ORDER BY sort_order ASC, id ASC";

inline constexpr std::string_view INSERT =
    "INSERT INTO project_task (project_id, created_by, assignee_id, title, "
    "status, priority, due_at, sort_order) VALUES (?, ?, ?, ?, ?, ?, ?, ?)";

inline constexpr std::string_view UPDATE_PREFIX = "UPDATE project_task SET ";
inline constexpr std::string_view UPDATE_COL_TITLE = "title = ?";
inline constexpr std::string_view UPDATE_COL_STATUS = "status = ?";
inline constexpr std::string_view UPDATE_COL_PRIORITY = "priority = ?";
inline constexpr std::string_view UPDATE_COL_ASSIGNEE_ID = "assignee_id = ?";
inline constexpr std::string_view UPDATE_COL_DUE_AT = "due_at = ?";
inline constexpr std::string_view UPDATE_COL_SORT_ORDER = "sort_order = ?";
inline constexpr std::string_view UPDATE_SUFFIX =
    ", updated_at = strftime('%s', 'now') "
    "WHERE id = ? AND deleted_at IS NULL";

inline constexpr std::string_view REMOVE =
    "UPDATE project_task SET deleted_at = strftime('%s', 'now'), "
    "updated_at = strftime('%s', 'now') "
    "WHERE id = ? AND deleted_at IS NULL";

} // namespace project_task_query

struct ProjectTaskCreateInput
{
  int64_t projectId{0};
  std::optional<int64_t> createdBy;
  std::optional<int64_t> assigneeId;
  std::string title;
  std::string status;
  std::string priority;
  std::optional<int64_t> dueAt;
  double sortOrder{0.0};
};

struct ProjectTaskUpdateInput
{
  std::optional<std::string> title;
  std::optional<std::string> status;
  std::optional<std::string> priority;
  std::optional<int64_t> assigneeId;
  std::optional<int64_t> dueAt;
  std::optional<double> sortOrder;
};
