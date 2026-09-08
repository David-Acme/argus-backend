#pragma once

#include <cstdint>
#include <drogon/orm/Field.h>
#include <drogon/orm/Row.h>
#include <json/value.h>
#include <optional>
#include <string>

// Calendar event row; endsAt absent means a point in time, not a span.
struct CalendarEventSchema
{
  int64_t id{0};
  std::optional<int64_t> createdBy;
  int64_t ownerId{0};
  std::optional<int64_t> projectId;
  std::string title;
  std::string description;
  std::string location;
  std::string color;
  int64_t startsAt{0};
  std::optional<int64_t> endsAt;
  bool isAllDay{false};
  std::optional<std::string> recurrenceRule;
  int64_t createdAt{0};
  std::optional<int64_t> updatedAt;
  std::optional<int64_t> deletedAt;

  CalendarEventSchema() = default;
  explicit CalendarEventSchema(const drogon::orm::Row& row);
  Json::Value toJson() const;
};
