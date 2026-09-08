#pragma once

#include <cstdint>
#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <json/value.h>
#include <optional>
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

  RefreshTokenSchema() = default;
  explicit RefreshTokenSchema(const drogon::orm::Row& row);
  Json::Value toJson() const;
};
