#pragma once
#include <string>
#include <string_view>

namespace notification_token_query
{
inline constexpr std::string_view RELEASE_TOKEN =
    "DELETE FROM notification_token WHERE token = ? "
    "AND NOT (user_id = ? AND device_hash = ?)";

inline constexpr std::string_view UPSERT =
    "INSERT INTO notification_token (user_id, device_hash, token, platform, "
    "lang, session_id, provider, public_key, key_id) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) "
    "ON CONFLICT(user_id, device_hash) DO UPDATE SET "
    "token = excluded.token, platform = excluded.platform, "
    "lang = excluded.lang, session_id = excluded.session_id, "
    "provider = excluded.provider, public_key = excluded.public_key, "
    "key_id = excluded.key_id, is_active = 1, "
    "updated_at = strftime('%s', 'now')";

inline constexpr std::string_view FIND_BY_USER =
    "SELECT * FROM notification_token WHERE user_id = ? AND is_active = 1";

inline constexpr std::string_view DELETE_FOR_SESSION =
    "DELETE FROM notification_token WHERE user_id = ? AND session_id = ? "
    "AND session_id <> ''";

inline constexpr std::string_view DELETE_FOR_USER =
    "DELETE FROM notification_token WHERE user_id = ?";

inline constexpr std::string_view FIND_PUSH_BY_USER =
    "SELECT * FROM notification_token WHERE user_id = ? AND is_active = 1 "
    "AND provider <> '' AND public_key <> '' AND key_id <> ''";

inline constexpr std::string_view DELETE_OWN_TOKEN =
    "DELETE FROM notification_token WHERE user_id = ? AND token = ?";

inline constexpr std::string_view DELETE_BY_TOKEN =
    "DELETE FROM notification_token WHERE token = ?";
}

struct NotificationTokenSessionInput
{
  int64_t userId{0};
  std::string sessionId;
};

struct NotificationTokenCreateInput
{
  int64_t userId{0};
  std::string deviceHash;
  std::string token;
  std::string platform;
  std::string lang;
  std::string sessionId;
  std::string provider;
  std::string publicKey;
  std::string keyId;
};

struct NotificationTokenRemoveInput
{
  int64_t userId{0};
  std::string token;
};
