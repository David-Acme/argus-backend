#pragma once

#include <cstdint>
#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <json/value.h>
#include <optional>
#include <shared/enums.hxx>
#include <string>

struct ReminderDetailSchema
{
  int64_t id{0};
  int64_t reminderId{0};
  std::optional<int64_t> createdBy;
  std::string content;
  ReminderDetailStatus status{ReminderDetailStatus::Pending};
  std::string filePaths;
  int64_t createdAt{0};
  std::optional<int64_t> updatedAt;
  std::optional<int64_t> deletedAt;

  ReminderDetailSchema() = default;
  explicit ReminderDetailSchema(const drogon::orm::Row& row);
  Json::Value toJson() const;
};
