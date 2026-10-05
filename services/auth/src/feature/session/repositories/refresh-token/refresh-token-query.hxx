#pragma once

#include <array>
#include <auth/session-platform.hxx>
#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <string>
#include <string_view>

namespace refresh_token_query
{

inline constexpr std::string_view FIND_BY_ACCESS_TOKEN =
    "SELECT * FROM refresh_token "
    "WHERE user_id = ? AND access_token IN (?, ?) AND is_valid = 1 "
    "AND is_used = 0";

inline constexpr std::string_view FIND_BY_REFRESH_TOKEN =
    "SELECT * FROM refresh_token "
    "WHERE user_id = ? AND refresh_token IN (?, ?) AND is_valid = 1 "
    "AND is_used = 0";

inline constexpr std::string_view FIND_ACTIVE_BY_SESSION =
    "SELECT * FROM refresh_token "
    "WHERE user_id = ? AND session_id = ? AND is_valid = 1 AND is_used = 0 "
    "ORDER BY id DESC LIMIT 1";

inline constexpr std::string_view FIND_ACTIVE_BY_PREVIOUS =
    "SELECT * FROM refresh_token "
    "WHERE user_id = ? AND previous_refresh_token = ? AND is_valid = 1 "
    "AND is_used = 0 ORDER BY id DESC LIMIT 1";

inline constexpr std::string_view LIST_ACTIVE =
    "SELECT * FROM refresh_token "
    "WHERE user_id = ? AND is_valid = 1 AND is_used = 0 AND expires_at > ? "
    "AND session_id <> '' ORDER BY last_seen_at DESC, id DESC";

inline constexpr std::string_view LIST_ALL_ACTIVE =
    "SELECT * FROM refresh_token "
    "WHERE is_valid = 1 AND is_used = 0 AND expires_at > ? "
    "AND session_id <> '' ORDER BY user_id, last_seen_at DESC, id DESC";

inline constexpr std::string_view INSERT =
    "INSERT INTO refresh_token "
    "(user_id, access_token, refresh_token, device_hash, user_agent, "
    "expires_at, created_at, session_id, platform, device_name, "
    "session_created_at, last_seen_at, previous_refresh_token, network_hash) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)";

inline constexpr std::string_view MARK_USED =
    "UPDATE refresh_token SET is_used = 1 "
    "WHERE id = ? AND is_valid = 1 AND is_used = 0";

inline constexpr std::string_view INVALIDATE_ALL_USER =
    "UPDATE refresh_token SET is_valid = 0 "
    "WHERE user_id = ? AND is_valid = 1";

inline constexpr std::string_view INVALIDATE_SESSION =
    "UPDATE refresh_token SET is_valid = 0 "
    "WHERE user_id = ? AND session_id = ? AND is_valid = 1";

inline constexpr std::string_view INVALIDATE_OTHER_SESSIONS =
    "UPDATE refresh_token SET is_valid = 0 "
    "WHERE user_id = ? AND session_id <> ? AND is_valid = 1";

inline constexpr std::string_view TOUCH =
    "UPDATE refresh_token SET last_seen_at = ? "
    "WHERE id = ? AND last_seen_at <= ?";

inline constexpr std::string_view ADOPT_LEGACY_SESSIONS =
    "UPDATE refresh_token SET session_id = lower(hex(randomblob(16))), "
    "session_created_at = CASE WHEN session_created_at = 0 THEN created_at "
    "ELSE session_created_at END, "
    "last_seen_at = CASE WHEN last_seen_at = 0 THEN created_at "
    "ELSE last_seen_at END "
    "WHERE user_id = ? AND session_id = ''";

inline constexpr std::string_view BACKFILL_LEGACY_SESSIONS =
    "UPDATE refresh_token SET session_id = lower(hex(randomblob(16))), "
    "session_created_at = CASE WHEN session_created_at = 0 THEN created_at "
    "ELSE session_created_at END, "
    "last_seen_at = CASE WHEN last_seen_at = 0 THEN created_at "
    "ELSE last_seen_at END "
    "WHERE session_id = ''";

inline constexpr std::string_view PRUNE_STALE =
    "DELETE FROM refresh_token WHERE user_id = ? "
    "AND (is_used = 1 OR is_valid = 0 OR expires_at <= strftime('%s', 'now'))";

inline constexpr std::string_view COUNT_TABLE =
    "SELECT COUNT(*) AS total FROM sqlite_master "
    "WHERE type = 'table' AND name = 'refresh_token'";

inline constexpr std::string_view TABLE_COLUMNS =
    "SELECT name FROM pragma_table_info('refresh_token')";

struct AddedColumn
{
  std::string_view name;
  std::string_view statement;
};

inline constexpr std::array<AddedColumn, 7> ADDED_COLUMNS{{
    {.name = "session_id",
     .statement = "ALTER TABLE refresh_token ADD COLUMN session_id TEXT NOT "
                  "NULL DEFAULT ''"},
    {.name = "platform",
     .statement = "ALTER TABLE refresh_token ADD COLUMN platform TEXT NOT NULL "
                  "DEFAULT 'unknown' CHECK (platform IN ('unknown', 'android', "
                  "'ios', 'desktop', 'web'))"},
    {.name = "device_name",
     .statement = "ALTER TABLE refresh_token ADD COLUMN device_name TEXT NOT "
                  "NULL DEFAULT ''"},
    {.name = "session_created_at",
     .statement = "ALTER TABLE refresh_token ADD COLUMN session_created_at "
                  "INTEGER NOT NULL DEFAULT 0"},
    {.name = "last_seen_at",
     .statement = "ALTER TABLE refresh_token ADD COLUMN last_seen_at INTEGER "
                  "NOT NULL DEFAULT 0"},
    {.name = "previous_refresh_token",
     .statement = "ALTER TABLE refresh_token ADD COLUMN previous_refresh_token "
                  "TEXT NOT NULL DEFAULT ''"},
    {.name = "network_hash",
     .statement = "ALTER TABLE refresh_token ADD COLUMN network_hash TEXT NOT "
                  "NULL DEFAULT ''"},
}};

}

struct RefreshTokenCreateInput
{
  int64_t userId{0};
  std::string accessToken;
  std::string refreshToken;
  std::string deviceHash;
  std::string userAgent;
  int64_t expiresAt{0};
  std::string sessionId;
  SessionPlatform platform{SessionPlatform::Unknown};
  std::string deviceName;
  int64_t sessionCreatedAt{0};
  std::string previousRefreshHash;
  std::string networkHash;
  drogon::orm::DbClient* client{nullptr};
};

struct SessionLookupInput
{
  int64_t userId{0};
  std::string sessionId;
  drogon::orm::DbClient* client{nullptr};
};

struct ActiveSessionsInput
{
  int64_t userId{0};
  int64_t now{0};
  drogon::orm::DbClient* client{nullptr};
};

struct SessionTouchInput
{
  int64_t rowId{0};
  int64_t now{0};
  int64_t throttleSeconds{0};
};
