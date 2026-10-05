#pragma once

#include <array>
#include <auth/session-origin.hxx>
#include <auth/session-platform.hxx>
#include <cstdint>
#include <drogon/orm/DbClient.h>
#include <string>
#include <string_view>

namespace device_login_challenge_query
{

inline constexpr std::string_view FIND_BY_CHALLENGE_ID =
    "SELECT * FROM device_login_challenge WHERE challenge_id = ?";

inline constexpr std::string_view INSERT =
    "INSERT INTO device_login_challenge "
    "(challenge_id, device_hash, user_agent, expires_at, platform, "
    "device_name, poll_hash, origin, ip_address) "
    "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)";

inline constexpr std::string_view MARK_APPROVED =
    "UPDATE device_login_challenge "
    "SET status = 'approved', user_id = ?, access_token = ?, "
    "refresh_token = ? "
    "WHERE challenge_id = ? AND status = 'pending'";

inline constexpr std::string_view CLAIM_APPROVED =
    "UPDATE device_login_challenge "
    "SET status = 'expired', access_token = NULL, refresh_token = NULL "
    "WHERE challenge_id = ? AND status = 'approved' AND expires_at > ?";

inline constexpr std::string_view DELETE_BY_CHALLENGE_ID =
    "DELETE FROM device_login_challenge WHERE challenge_id = ?";

inline constexpr std::string_view DELETE_EXPIRED =
    "DELETE FROM device_login_challenge WHERE expires_at <= ?";

inline constexpr std::string_view COUNT_TABLE =
    "SELECT COUNT(*) AS total FROM sqlite_master "
    "WHERE type = 'table' AND name = 'device_login_challenge'";

inline constexpr std::string_view TABLE_COLUMNS =
    "SELECT name FROM pragma_table_info('device_login_challenge')";

struct AddedColumn
{
  std::string_view name;
  std::string_view statement;
};

inline constexpr std::array<AddedColumn, 5> ADDED_COLUMNS{{
    {.name = "platform",
     .statement = "ALTER TABLE device_login_challenge ADD COLUMN platform TEXT "
                  "NOT NULL DEFAULT 'unknown' CHECK (platform IN ('unknown', "
                  "'android', 'ios', 'desktop', 'web'))"},
    {.name = "device_name",
     .statement = "ALTER TABLE device_login_challenge ADD COLUMN device_name "
                  "TEXT NOT NULL DEFAULT ''"},
    {.name = "poll_hash",
     .statement = "ALTER TABLE device_login_challenge ADD COLUMN poll_hash "
                  "TEXT NOT NULL DEFAULT ''"},
    {.name = "origin",
     .statement = "ALTER TABLE device_login_challenge ADD COLUMN origin TEXT "
                  "NOT NULL DEFAULT 'unknown' CHECK (origin IN ('unknown', "
                  "'lan', 'tunnel', 'loopback', 'external'))"},
    {.name = "ip_address",
     .statement = "ALTER TABLE device_login_challenge ADD COLUMN ip_address "
                  "TEXT NOT NULL DEFAULT ''"},
}};

}

struct DeviceLoginChallengeCreateInput
{
  std::string challengeId;
  std::string deviceHash;
  std::string userAgent;
  int64_t expiresAt{0};
  SessionPlatform platform{SessionPlatform::Unknown};
  std::string deviceName;
  std::string pollHash;
  SessionOrigin origin{SessionOrigin::Unknown};
  std::string ipAddress;
};

struct DeviceLoginChallengeMarkApprovedInput
{
  std::string challengeId;
  int64_t userId{0};
  std::string accessToken;
  std::string refreshToken;
  drogon::orm::DbClient* client{nullptr};
};
