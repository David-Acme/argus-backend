#pragma once

#include <cstdint>
#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <optional>
#include <string>

struct ClipPreviewCapabilitySchema
{
  int64_t id{0};
  std::string tokenHash;
  int64_t clipId{0};
  int64_t requesterUserId{0};
  int64_t expiresAt{0};
  std::optional<int64_t> consumedAt;
  int64_t createdAt{0};

  ClipPreviewCapabilitySchema() = default;
  explicit ClipPreviewCapabilitySchema(const drogon::orm::Row& row);
};
