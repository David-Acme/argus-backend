#pragma once
#include <cstdint>
#include <optional>
#include <shared/enums.hxx>
#include <string>
#include <string_view>

namespace reminder_detail_query
{
inline constexpr std::string_view FIND_BY_ID =
    "SELECT * FROM reminder_detail WHERE id = ? AND deleted_at IS NULL";
inline constexpr std::string_view FIND_BY_REMINDER =
    "SELECT * FROM reminder_detail WHERE reminder_id = ? AND deleted_at IS "
    "NULL "
    "ORDER BY created_at ASC";
inline constexpr std::string_view FIND =
    "SELECT * FROM reminder_detail WHERE deleted_at IS NULL "
    "AND created_at >= ? AND created_at <= ? ORDER BY created_at ASC LIMIT 200";
inline constexpr std::string_view FIND_FROM =
    "SELECT * FROM reminder_detail WHERE deleted_at IS NULL "
    "AND created_at >= ? ORDER BY created_at ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED =
    "SELECT * FROM reminder_detail WHERE deleted_at IS NOT NULL "
    "AND deleted_at >= ? AND deleted_at <= ? ORDER BY deleted_at ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_FROM =
    "SELECT * FROM reminder_detail WHERE deleted_at IS NOT NULL "
    "AND deleted_at >= ? ORDER BY deleted_at ASC LIMIT 200";
inline constexpr std::string_view FIND_ALL =
    "SELECT * FROM reminder_detail WHERE deleted_at IS NULL "
    "ORDER BY created_at ASC LIMIT 200";
inline constexpr std::string_view FIND_DELETED_ALL =
    "SELECT * FROM reminder_detail WHERE deleted_at IS NOT NULL "
    "ORDER BY deleted_at ASC LIMIT 200";
inline constexpr std::string_view FIND_LAST =
    "SELECT * FROM reminder_detail WHERE deleted_at IS NULL "
    "ORDER BY created_at DESC LIMIT 1";
inline constexpr std::string_view FIND_LAST_DELETED =
    "SELECT * FROM reminder_detail WHERE deleted_at IS NOT NULL "
    "ORDER BY deleted_at DESC LIMIT 1";
inline constexpr std::string_view INSERT =
    "INSERT INTO reminder_detail (reminder_id, created_by, content, status, "
    "file_paths) VALUES (?, ?, ?, ?, ?)";
inline constexpr std::string_view UPDATE =
    "UPDATE reminder_detail SET content = ?, status = ?, file_paths = ?, "
    "updated_at = strftime('%s', 'now') WHERE id = ? AND deleted_at IS NULL";
inline constexpr std::string_view REMOVE =
    "UPDATE reminder_detail SET deleted_at = strftime('%s', 'now'), "
    "updated_at = strftime('%s', 'now') WHERE id = ? AND deleted_at IS NULL";
} // namespace reminder_detail_query

struct ReminderDetailCreateInput
{
  int64_t reminderId{0};
  std::optional<int64_t> createdBy;
  std::string content;
  ReminderDetailStatus status{ReminderDetailStatus::Pending};
  std::string filePaths;
};

struct ReminderDetailUpdateInput
{
  std::string content;
  ReminderDetailStatus status{ReminderDetailStatus::Pending};
  std::string filePaths;
};

