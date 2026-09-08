#pragma once

#include <cstdint>
#include <drogon/orm/Row.h>
#include <optional>
#include <string>

struct DeviceLoginChallengeSchema
{
  int64_t id{0};
  std::string challengeId;
  std::string deviceHash;
  std::string userAgent;
  std::string status{"pending"};
  std::optional<int64_t> userId;
  std::string accessToken;
  std::string refreshToken;
  int64_t expiresAt{0};
  int64_t createdAt{0};

  DeviceLoginChallengeSchema() = default;
  explicit DeviceLoginChallengeSchema(const drogon::orm::Row& row);
};