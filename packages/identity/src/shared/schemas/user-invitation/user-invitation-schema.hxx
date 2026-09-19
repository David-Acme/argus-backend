#pragma once

#include <cstdint>
#include <drogon/orm/Row.h>
#include <json/value.h>
#include <optional>
#include <shared/enums.hxx>
#include <string>

struct UserInvitationSchema
{
  int64_t id{0};
  std::string tokenHash;
  UserRole role{UserRole::Guest};
  int maxRedemptions{1};
  int redemptionCount{0};
  int64_t expiresAt{0};
  int64_t createdBy{0};
  std::optional<int64_t> revokedAt;
  std::optional<int64_t> revokedBy;
  int64_t createdAt{0};
  std::optional<int64_t> updatedAt;

  UserInvitationSchema() = default;
  explicit UserInvitationSchema(const drogon::orm::Row& row);
  Json::Value toJson() const;
};
