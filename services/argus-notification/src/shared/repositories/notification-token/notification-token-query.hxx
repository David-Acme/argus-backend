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
} // namespace notification_token_query

struct NotificationTokenCreateInput
{
  int64_t userId{0};
  std::string deviceHash;
  std::string token;
  std::string platform;
  std::string lang;
};
