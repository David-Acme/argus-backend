#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace refresh_token_query
{

inline constexpr std::string_view FIND_BY_ID =
    "SELECT * FROM refresh_token WHERE id = ?";

inline constexpr std::string_view FIND_BY_ACCESS_TOKEN =
    "SELECT * FROM refresh_token "
    "WHERE user_id = ? AND access_token = ?";

inline constexpr std::string_view FIND_BY_REFRESH_TOKEN =
    "SELECT * FROM refresh_token "
    "WHERE user_id = ? AND refresh_token = ? AND is_valid = 1 AND is_used = 0";

inline constexpr std::string_view INSERT =
    "INSERT INTO refresh_token "
    "(user_id, access_token, refresh_token, device_hash, user_agent, "
    "expires_at) "
    "VALUES (?, ?, ?, ?, ?, ?)";

inline constexpr std::string_view INVALIDATE =
    "UPDATE refresh_token SET is_valid = 0 WHERE id = ?";

inline constexpr std::string_view MARK_USED =
    "UPDATE refresh_token SET is_used = 1 WHERE id = ?";

inline constexpr std::string_view INVALIDATE_ALL_USER =
    "UPDATE refresh_token SET is_valid = 0 "
    "WHERE user_id = ? AND is_valid = 1";

inline constexpr std::string_view DELETE_EXPIRED =
    "DELETE FROM refresh_token WHERE expires_at < ?";

} // namespace refresh_token_query

struct RefreshTokenCreateInput
{
  int64_t userId{0};
  std::string accessToken;
  std::string refreshToken;
  std::string deviceHash;
  std::string userAgent;
  int64_t expiresAt{0};
};

