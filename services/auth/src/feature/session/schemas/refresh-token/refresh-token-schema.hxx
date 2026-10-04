#pragma once

#include <auth/session-platform.hxx>
#include <cstdint>
#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <string>

struct RefreshTokenSchema
{
  int64_t id{0};
  int64_t userId{0};
  std::string accessToken;
  std::string refreshToken;
  std::string deviceHash;
  std::string userAgent;
  bool isValid{true};
  bool isUsed{false};
  int64_t expiresAt{0};
  int64_t createdAt{0};
  std::string sessionId;
  SessionPlatform platform{SessionPlatform::Unknown};
  std::string deviceName;
  int64_t sessionCreatedAt{0};
  int64_t lastSeenAt{0};
  std::string previousRefreshToken;

  RefreshTokenSchema() = default;
  explicit RefreshTokenSchema(const drogon::orm::Row& row);
};
