#pragma once

#include <cstdint>
#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <json/value.h>
#include <optional>
#include <string>

struct ProjectSchema
{
  int64_t id{0};
  int64_t ownerId{0};
  std::string name;
  std::string description;
  std::string status;
  std::string color;
  std::optional<int64_t> startsAt;
  std::optional<int64_t> targetAt;
  int64_t createdAt{0};
  std::optional<int64_t> updatedAt;
  std::optional<int64_t> deletedAt;

  ProjectSchema() = default;
  explicit ProjectSchema(const drogon::orm::Row& row);
  Json::Value toJson() const;
};
