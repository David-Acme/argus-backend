#include "refresh-token-schema.hxx"

RefreshTokenSchema::RefreshTokenSchema(const drogon::orm::Row& row)
{
  id = static_cast<int64_t>(row["id"].as<long long>());
  userId = static_cast<int64_t>(row["user_id"].as<long long>());
  accessToken = row["access_token"].as<std::string>();
  refreshToken = row["refresh_token"].as<std::string>();
  deviceHash = row["device_hash"].as<std::string>();
  userAgent = row["user_agent"].as<std::string>();
  isValid = row["is_valid"].as<int>() != 0;
  isUsed = row["is_used"].as<int>() != 0;
  expiresAt = static_cast<int64_t>(row["expires_at"].as<long long>());
  createdAt = static_cast<int64_t>(row["created_at"].as<long long>());
  sessionId = row["session_id"].as<std::string>();
  platform = sessionPlatformFromString(row["platform"].as<std::string>());
  deviceName = row["device_name"].as<std::string>();
  sessionCreatedAt =
      static_cast<int64_t>(row["session_created_at"].as<long long>());
  lastSeenAt = static_cast<int64_t>(row["last_seen_at"].as<long long>());
  previousRefreshToken = row["previous_refresh_token"].as<std::string>();
}
