#include "device-login-challenge-schema.hxx"

#include <drogon/orm/Field.h>

DeviceLoginChallengeSchema::DeviceLoginChallengeSchema(const drogon::orm::Row& row)
{
  id = static_cast<int64_t>(row["id"].as<long long>());
  challengeId = row["challenge_id"].as<std::string>();
  deviceHash = row["device_hash"].as<std::string>();
  userAgent = row["user_agent"].as<std::string>();
  status = row["status"].as<std::string>();
  if (!row["user_id"].isNull())
    userId = static_cast<int64_t>(row["user_id"].as<long long>());
  if (!row["access_token"].isNull())
    accessToken = row["access_token"].as<std::string>();
  if (!row["refresh_token"].isNull())
    refreshToken = row["refresh_token"].as<std::string>();
  expiresAt = static_cast<int64_t>(row["expires_at"].as<long long>());
  createdAt = static_cast<int64_t>(row["created_at"].as<long long>());
}