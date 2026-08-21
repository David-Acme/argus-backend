#pragma once

#include <cstdint>
#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <json/value.h>
#include <optional>
#include <string>

struct ProjectTaskSchema
{
  int64_t id{0};
  int64_t projectId{0};
  std::optional<int64_t> createdBy;
  std::optional<int64_t> assigneeId;
  std::string title;
  std::string status;
  std::string priority;
  std::optional<int64_t> dueAt;
  double sortOrder{0.0};
  int64_t createdAt{0};
  std::optional<int64_t> updatedAt;
  std::optional<int64_t> deletedAt;

  ProjectTaskSchema() = default;
  explicit ProjectTaskSchema(const drogon::orm::Row& row);
  Json::Value toJson() const;
};
