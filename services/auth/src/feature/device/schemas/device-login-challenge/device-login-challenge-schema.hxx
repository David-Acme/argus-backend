#pragma once

#include <auth/device-login-status.hxx>
#include <auth/session-platform.hxx>
#include <cstdint>
#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <optional>
#include <string>

struct DeviceLoginChallengeSchema
{
  int64_t id{0};
  std::string challengeId;
  std::string deviceHash;
  std::string userAgent;
  DeviceLoginStatus status{DeviceLoginStatus::Pending};
  std::optional<int64_t> userId;
  std::string accessToken;
  std::string refreshToken;
  int64_t expiresAt{0};
  int64_t createdAt{0};
  SessionPlatform platform{SessionPlatform::Unknown};
  std::string deviceName;

  DeviceLoginChallengeSchema() = default;
  explicit DeviceLoginChallengeSchema(const drogon::orm::Row& row);
};
