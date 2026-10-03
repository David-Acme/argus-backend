#pragma once

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

inline constexpr std::string_view INSERT =
    "INSERT INTO refresh_token "
    "(user_id, access_token, refresh_token, device_hash, user_agent, "
    "expires_at) "
    "VALUES (?, ?, ?, ?, ?, ?)";

inline constexpr std::string_view MARK_USED =
    "UPDATE refresh_token SET is_used = 1 "
    "WHERE id = ? AND is_valid = 1 AND is_used = 0";

inline constexpr std::string_view INVALIDATE_ALL_USER =
    "UPDATE refresh_token SET is_valid = 0 "
    "WHERE user_id = ? AND is_valid = 1";

inline constexpr std::string_view PRUNE_STALE =
    "DELETE FROM refresh_token WHERE user_id = ? "
    "AND (is_used = 1 OR is_valid = 0 OR expires_at <= strftime('%s', 'now'))";

}

struct RefreshTokenCreateInput
{
  int64_t userId{0};
  std::string accessToken;
  std::string refreshToken;
  std::string deviceHash;
  std::string userAgent;
  int64_t expiresAt{0};
  drogon::orm::DbClient* client{nullptr};
};
