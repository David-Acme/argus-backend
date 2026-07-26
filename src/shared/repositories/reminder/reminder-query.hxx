#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace reminder_query
{

inline constexpr std::string_view FIND_BY_ID =
    "SELECT * FROM reminder WHERE id = ? AND deleted_at IS NULL";

inline constexpr std::string_view FIND =
    "SELECT * FROM reminder "
    "WHERE deleted_at IS NULL AND created_at >= ? AND created_at <= ? "
    "ORDER BY created_at ASC LIMIT 200";

inline constexpr std::string_view FIND_FROM =
    "SELECT * FROM reminder "
    "WHERE deleted_at IS NULL AND created_at >= ? "
    "ORDER BY created_at ASC LIMIT 200";

inline constexpr std::string_view FIND_DELETED =
    "SELECT * FROM reminder "
    "WHERE deleted_at IS NOT NULL AND deleted_at >= ? AND deleted_at <= ? "
    "ORDER BY deleted_at ASC LIMIT 200";

inline constexpr std::string_view FIND_DELETED_FROM =
    "SELECT * FROM reminder "
    "WHERE deleted_at IS NOT NULL AND deleted_at >= ? "
    "ORDER BY deleted_at ASC LIMIT 200";

inline constexpr std::string_view FIND_ALL =
    "SELECT * FROM reminder "
    "WHERE deleted_at IS NULL "
    "ORDER BY created_at ASC LIMIT 200";

inline constexpr std::string_view FIND_DELETED_ALL =
    "SELECT * FROM reminder "
    "WHERE deleted_at IS NOT NULL "
    "ORDER BY deleted_at ASC LIMIT 200";

inline constexpr std::string_view FIND_LAST =
    "SELECT * FROM reminder "
    "WHERE deleted_at IS NULL "
    "ORDER BY created_at DESC LIMIT 1";

inline constexpr std::string_view FIND_LAST_DELETED =
    "SELECT * FROM reminder "
    "WHERE deleted_at IS NOT NULL "
    "ORDER BY deleted_at DESC LIMIT 1";

inline constexpr std::string_view FIND_BY_TARGET =
    "SELECT * FROM reminder WHERE target_user_id = ? AND deleted_at IS NULL "
    "ORDER BY scheduled_at ASC";

inline constexpr std::string_view INSERT =
    "INSERT INTO reminder (created_by, target_user_id, title, description, "
    "scheduled_at, recurrence_rule) "
    "VALUES (?, ?, ?, ?, ?, ?)";

inline constexpr std::string_view UPDATE =
    "UPDATE reminder SET title = ?, description = ?, scheduled_at = ?, "
    "recurrence_rule = ?, is_completed = ?, completed_at = ?, "
    "updated_at = strftime('%s', 'now') "
    "WHERE id = ? AND deleted_at IS NULL";

inline constexpr std::string_view REMOVE =
    "UPDATE reminder SET deleted_at = strftime('%s', 'now'), "
    "updated_at = strftime('%s', 'now') "
    "WHERE id = ? AND deleted_at IS NULL";

} // namespace reminder_query

struct ReminderCreateInput
{
  std::optional<int64_t> createdBy;
  int64_t targetUserId{0};
  std::string title;
  std::string description;
  int64_t scheduledAt{0};
  std::optional<std::string> recurrenceRule;
};

struct ReminderUpdateInput
{
  std::string title;
  std::string description;
  int64_t scheduledAt{0};
  std::optional<std::string> recurrenceRule;
  bool isCompleted{false};
  std::optional<int64_t> completedAt;
};

