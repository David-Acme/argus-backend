#pragma once
#include <string>
#include <string_view>

namespace notification_token_query
{
inline constexpr std::string_view UPSERT =
    "INSERT INTO notification_token (user_id, device_hash, token, platform, "
    "lang) VALUES (?, ?, ?, ?, ?) "
    "ON CONFLICT(user_id, device_hash) DO UPDATE SET "
    "token = excluded.token, platform = excluded.platform, "
    "lang = excluded.lang, is_active = 1, "
    "updated_at = strftime('%s', 'now')";

inline constexpr std::string_view FIND_BY_USER =
    "SELECT * FROM notification_token WHERE user_id = ? AND is_active = 1";

inline constexpr std::string_view DELETE_BY_DEVICE =
    "UPDATE notification_token SET is_active = 0, "
    "updated_at = strftime('%s', 'now') "
    "WHERE user_id = ? AND device_hash = ?";

inline constexpr std::string_view REMOVE_ALL_BY_USER =
    "UPDATE notification_token SET is_active = 0, "
    "updated_at = strftime('%s', 'now') WHERE user_id = ?";
} // namespace notification_token_query

struct NotificationTokenCreateInput
{
  int64_t userId{0};
  std::string deviceHash;
  std::string token;
  std::string platform;
  std::string lang;
};
