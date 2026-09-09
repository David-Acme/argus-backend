#pragma once

#include <cstdint>
#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <json/value.h>
#include <optional>
#include <string>

struct ReminderSchema
{
  int64_t id{0};
  std::optional<int64_t> createdBy;
  int64_t targetUserId{0};
  std::string title;
  std::string description;
  int64_t scheduledAt{0};
  std::optional<std::string> recurrenceRule;
  bool isCompleted{false};
  std::optional<int64_t> completedAt;
  int64_t createdAt{0};
  std::optional<int64_t> updatedAt;
  std::optional<int64_t> deletedAt;

  ReminderSchema() = default;
  explicit ReminderSchema(const drogon::orm::Row& row);
  Json::Value toJson() const;
};
