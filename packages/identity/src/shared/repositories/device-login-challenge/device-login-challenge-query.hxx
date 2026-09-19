#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace device_login_challenge_query
{

inline constexpr std::string_view FIND_BY_CHALLENGE_ID =
    "SELECT * FROM device_login_challenge WHERE challenge_id = ?";

inline constexpr std::string_view INSERT =
    "INSERT INTO device_login_challenge "
    "(challenge_id, device_hash, user_agent, expires_at) "
    "VALUES (?, ?, ?, ?)";

inline constexpr std::string_view MARK_APPROVED =
    "UPDATE device_login_challenge "
    "SET status = 'approved', user_id = ?, access_token = ?, "
    "refresh_token = ? "
    "WHERE challenge_id = ? AND status = 'pending'";

inline constexpr std::string_view DELETE_BY_CHALLENGE_ID =
    "DELETE FROM device_login_challenge WHERE challenge_id = ?";

inline constexpr std::string_view DELETE_EXPIRED =
    "DELETE FROM device_login_challenge WHERE expires_at < ?";

} // namespace device_login_challenge_query

struct DeviceLoginChallengeCreateInput
{
  std::string challengeId;
  std::string deviceHash;
  std::string userAgent;
  int64_t expiresAt{0};
};

struct DeviceLoginChallengeMarkApprovedInput
{
  std::string challengeId;
  int64_t userId{0};
  std::string accessToken;
  std::string refreshToken;
};
