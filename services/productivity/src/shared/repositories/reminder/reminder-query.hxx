#pragma once

#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <optional>
#include <string>
#include <string_view>

namespace reminder_query
{

inline constexpr int OWNERSHIP_PLACEHOLDERS = 1;

inline constexpr std::string_view FIND_BY_ID =
    "SELECT * FROM reminder WHERE id = ? AND deleted_at IS NULL";

inline constexpr std::string_view FIND_OWNED =
    "SELECT * FROM reminder WHERE id = ? AND target_user_id = ? "
    "AND deleted_at IS NULL";

inline constexpr std::string_view FIND =
    "SELECT * FROM reminder "
    "WHERE deleted_at IS NULL AND created_at >= ? AND created_at <= ? "
    "AND target_user_id = ? "
    "ORDER BY created_at ASC, id ASC LIMIT 200";

inline constexpr std::string_view FIND_FROM =
    "SELECT * FROM reminder "
    "WHERE deleted_at IS NULL AND created_at >= ? AND target_user_id = ? "
    "ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_AFTER =
    "SELECT * FROM reminder WHERE deleted_at IS NULL AND "
    "(created_at > ? OR (created_at = ? AND id > ?)) AND created_at <= ? "
    "AND target_user_id = ? "
    "ORDER BY created_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_AFTER_FROM =
    "SELECT * FROM reminder WHERE deleted_at IS NULL AND "
    "(created_at > ? OR (created_at = ? AND id > ?)) AND target_user_id = ? "
    "ORDER BY created_at ASC, id ASC LIMIT 200";

inline constexpr std::string_view FIND_DELETED =
    "SELECT * FROM reminder "
    "WHERE deleted_at IS NOT NULL AND deleted_at < strftime('%s', 'now') - 1 AND deleted_at >= ? AND deleted_at <= ? "
    "AND target_user_id = ? "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";

inline constexpr std::string_view FIND_DELETED_FROM =
    "SELECT * FROM reminder "
    "WHERE deleted_at IS NOT NULL AND deleted_at < strftime('%s', 'now') - 1 AND deleted_at >= ? "
    "AND target_user_id = ? "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_AFTER =
    "SELECT * FROM reminder WHERE deleted_at IS NOT NULL AND deleted_at < strftime('%s', 'now') - 1 AND "
    "(deleted_at > ? OR (deleted_at = ? AND id > ?)) AND deleted_at <= ? "
    "AND target_user_id = ? "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_AFTER_FROM =
    "SELECT * FROM reminder WHERE deleted_at IS NOT NULL AND deleted_at < strftime('%s', 'now') - 1 AND "
    "(deleted_at > ? OR (deleted_at = ? AND id > ?)) AND target_user_id = ? "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";

inline constexpr std::string_view FIND_ALL =
    "SELECT * FROM reminder "
    "WHERE deleted_at IS NULL AND target_user_id = ? "
    "ORDER BY created_at ASC, id ASC LIMIT 200";

inline constexpr std::string_view FIND_DELETED_ALL =
    "SELECT * FROM reminder "
    "WHERE deleted_at IS NOT NULL AND deleted_at < strftime('%s', 'now') - 1 "
    "AND target_user_id = ? "
    "ORDER BY deleted_at ASC, id ASC LIMIT 200";

inline constexpr std::string_view FIND_LAST =
    "SELECT * FROM reminder "
    "WHERE deleted_at IS NULL AND target_user_id = ? "
    "ORDER BY created_at DESC, id DESC LIMIT 1";

inline constexpr std::string_view FIND_LAST_DELETED =
    "SELECT * FROM reminder "
    "WHERE deleted_at IS NOT NULL AND deleted_at < strftime('%s', 'now') - 1 "
    "AND target_user_id = ? "
    "ORDER BY deleted_at DESC, id DESC LIMIT 1";

inline constexpr std::string_view FIND_OVERDUE_SERIES =
    "SELECT * FROM reminder WHERE deleted_at IS NULL AND is_completed = 0 "
    "AND recurrence_rule IS NOT NULL AND scheduled_at <= ? "
    "ORDER BY scheduled_at ASC, id ASC LIMIT ?";

inline constexpr std::string_view FIND_BY_TARGET =
    "SELECT * FROM reminder WHERE target_user_id = ? AND deleted_at IS NULL "
    "AND (? = 1 OR is_completed = 0) "
    "ORDER BY scheduled_at ASC, id ASC LIMIT ?";

inline constexpr std::string_view INSERT =
    "INSERT INTO reminder (created_by, target_user_id, title, description, "
    "scheduled_at, recurrence_rule, recurrence_anchor) "
    "VALUES (?, ?, ?, ?, ?, ?, ?)";

inline constexpr std::string_view ADD_RECURRENCE_ANCHOR_COLUMN =
    "ALTER TABLE reminder ADD COLUMN recurrence_anchor INTEGER";
inline constexpr std::string_view ADD_RECURRENCE_EXCEPTIONS_COLUMN =
    "ALTER TABLE reminder ADD COLUMN recurrence_exceptions TEXT NOT NULL DEFAULT '[]'";
inline constexpr std::string_view BACKFILL_RECURRENCE_ANCHOR =
    "UPDATE reminder SET recurrence_anchor = scheduled_at "
    "WHERE recurrence_rule IS NOT NULL AND recurrence_anchor IS NULL";

inline constexpr std::string_view UPDATE_PREFIX = "UPDATE reminder SET ";
inline constexpr std::string_view UPDATE_COL_TITLE = "title = ?";
inline constexpr std::string_view UPDATE_COL_DESCRIPTION = "description = ?";
inline constexpr std::string_view UPDATE_COL_SCHEDULED_AT = "scheduled_at = ?";
inline constexpr std::string_view UPDATE_COL_RECURRENCE_RULE = "recurrence_rule = ?";
inline constexpr std::string_view UPDATE_COL_RECURRENCE_RULE_NULL = "recurrence_rule = NULL";
inline constexpr std::string_view UPDATE_COL_RECURRENCE_ANCHOR = "recurrence_anchor = ?";
inline constexpr std::string_view UPDATE_COL_RECURRENCE_ANCHOR_NULL = "recurrence_anchor = NULL";
inline constexpr std::string_view UPDATE_COL_RECURRENCE_EXCEPTIONS = "recurrence_exceptions = ?";
inline constexpr std::string_view UPDATE_COL_IS_COMPLETED = "is_completed = ?";
inline constexpr std::string_view UPDATE_COL_COMPLETED_AT = "completed_at = ?";
inline constexpr std::string_view UPDATE_COL_COMPLETED_AT_NULL =
    "completed_at = NULL";
inline constexpr std::string_view UPDATE_SUFFIX =
    ", updated_at = strftime('%s', 'now') "
    "WHERE id = ? AND target_user_id = ? AND deleted_at IS NULL";

inline constexpr std::string_view REMOVE =
    "UPDATE reminder SET deleted_at = strftime('%s', 'now'), "
    "updated_at = strftime('%s', 'now') "
    "WHERE id = ? AND target_user_id = ? AND deleted_at IS NULL";

inline constexpr int64_t kListLimit = 200;

}

struct ReminderCreateInput
{
  std::optional<int64_t> createdBy;
  int64_t targetUserId{0};
  std::string title;
  std::string description;
  int64_t scheduledAt{0};
  std::optional<std::string> recurrenceRule;
  std::optional<int64_t> recurrenceAnchor;
  drogon::orm::DbClient* client{nullptr};
};

struct ReminderOwnedInput
{
  int64_t id{0};
  int64_t targetUserId{0};
  drogon::orm::DbClient* client{nullptr};
};

struct ReminderUpdateInput
{
  int64_t id{0};
  int64_t targetUserId{0};
  std::optional<std::string> title;
  std::optional<std::string> description;
  std::optional<int64_t> scheduledAt;
  std::optional<bool> isCompleted;
  std::optional<int64_t> completedAt;
  bool clearCompletedAt{false};
  std::optional<std::string> recurrenceRule;
  std::optional<int64_t> recurrenceAnchor;
  bool clearRecurrence{false};
  std::optional<std::string> recurrenceExceptions;
  drogon::orm::DbClient* client{nullptr};
};

struct ReminderOverdueInput
{
  int64_t before{0};
  int64_t limit{100};
};

struct ReminderListInput
{
  int64_t targetUserId{0};
  bool includeCompleted{true};
  int64_t limit{reminder_query::kListLimit};
};
