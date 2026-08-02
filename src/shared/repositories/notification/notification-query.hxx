#pragma once
#include <json/value.h>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace notification_query
{
inline constexpr std::string_view INSERT =
    "INSERT INTO notification (user_id, type, title, body, data) "
    "VALUES (?, ?, ?, ?, ?)";

inline constexpr std::string_view FIND_SYNC =
    "SELECT * FROM notification WHERE user_id = ? "
    "AND created_at >= ? AND created_at <= ? "
    "ORDER BY created_at ASC LIMIT ";

inline constexpr std::string_view FIND_SYNC_FROM =
    "SELECT * FROM notification WHERE user_id = ? "
    "AND created_at >= ? ORDER BY created_at ASC LIMIT ";

inline constexpr std::string_view FIND_SYNC_TO =
    "SELECT * FROM notification WHERE user_id = ? "
    "AND created_at <= ? ORDER BY created_at ASC LIMIT ";

inline constexpr std::string_view FIND_SYNC_ALL =
    "SELECT * FROM notification WHERE user_id = ? "
    "ORDER BY created_at ASC LIMIT ";

inline constexpr std::string_view FIND_LAST_SYNC =
    "SELECT * FROM notification WHERE user_id = ? "
    "ORDER BY created_at DESC LIMIT 1";

inline constexpr std::string_view MARK_READ =
    "UPDATE notification SET is_read = 1, read_at = strftime('%s', 'now') "
    "WHERE user_id = ? AND id IN (%1%)";
} // namespace notification_query

struct NotificationCreateInput
{
  int64_t userId{0};
  std::string type;
  std::string title;
  std::string body;
  Json::Value data;
};

struct NotificationSyncFilter
{
  int64_t userId{0};
  std::optional<int64_t> startTime;
  std::optional<int64_t> endTime;
};
