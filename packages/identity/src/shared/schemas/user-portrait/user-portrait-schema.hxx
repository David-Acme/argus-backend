#pragma once

#include <cstdint>
#include <drogon/orm/Row.h>
#include <optional>

struct UserPortraitSchema
{
  int64_t id{0};
  int64_t userId{0};
  int64_t fileId{0};
  int64_t createdAt{0};
  std::optional<int64_t> updatedAt;

  UserPortraitSchema() = default;
  explicit UserPortraitSchema(const drogon::orm::Row& row);
};
