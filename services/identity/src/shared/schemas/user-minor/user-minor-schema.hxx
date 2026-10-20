#pragma once

#include <cstdint>
#include <drogon/orm/Row.h>
#include <optional>

struct UserMinorSchema
{
  int64_t userId{0};
  bool guardianConsent{false};
  std::optional<int64_t> guardianId;
  int64_t setBy{0};
  int64_t setAt{0};

  UserMinorSchema() = default;
  explicit UserMinorSchema(const drogon::orm::Row& row);
};
