#pragma once

#include <cstdint>
#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <json/value.h>
#include <optional>
#include <string>

struct CalendarEventShareSchema
{
  int64_t id{0};
  int64_t calendarEventId{0};
  int64_t userId{0};
  /** `view` or `edit`; see `ShareAccess`. */
  std::string access;
  int64_t createdAt{0};
  std::optional<int64_t> updatedAt;
  std::optional<int64_t> deletedAt;

  CalendarEventShareSchema() = default;
  explicit CalendarEventShareSchema(const drogon::orm::Row& row);
  Json::Value toJson() const;
};
