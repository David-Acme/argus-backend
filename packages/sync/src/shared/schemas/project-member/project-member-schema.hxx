#pragma once

#include <cstdint>
#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <json/value.h>
#include <optional>
#include <string>

struct ProjectMemberSchema
{
  int64_t id{0};
  int64_t projectId{0};
  int64_t userId{0};
  std::string access;
  int64_t createdAt{0};
  std::optional<int64_t> updatedAt;
  std::optional<int64_t> deletedAt;

  ProjectMemberSchema() = default;
  explicit ProjectMemberSchema(const drogon::orm::Row& row);
  Json::Value toJson() const;
};
