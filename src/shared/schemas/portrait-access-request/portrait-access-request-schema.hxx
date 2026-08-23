#pragma once

#include <cstdint>
#include <drogon/orm/Row.h>
#include <optional>
#include <shared/enums.hxx>

struct PortraitAccessRequestSchema
{
  int64_t id{0};
  int64_t portraitUserId{0};
  int64_t requesterUserId{0};
  PortraitAccessRequestStatus status{PortraitAccessRequestStatus::Pending};
  std::optional<int64_t> resolvedBy;
  std::optional<int64_t> resolvedAt;
  int64_t createdAt{0};
  std::optional<int64_t> updatedAt;

  PortraitAccessRequestSchema() = default;
  explicit PortraitAccessRequestSchema(const drogon::orm::Row& row);
};
