#pragma once

#include <cstdint>
#include <drogon/orm/Row.h>
#include <optional>
#include <shared/enums.hxx>

struct PortraitAccessGrantSchema
{
  int64_t id{0};
  std::optional<int64_t> requestId;
  int64_t portraitUserId{0};
  int64_t granteeUserId{0};
  int64_t grantedBy{0};
  PortraitAccessGrantScope scope{PortraitAccessGrantScope::Temporary};
  std::optional<int64_t> expiresAt;
  std::optional<int64_t> revokedAt;
  std::optional<int64_t> revokedBy;
  int64_t createdAt{0};
  std::optional<int64_t> updatedAt;

  PortraitAccessGrantSchema() = default;
  explicit PortraitAccessGrantSchema(const drogon::orm::Row& row);
};
