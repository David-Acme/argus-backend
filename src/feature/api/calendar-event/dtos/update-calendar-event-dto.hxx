#pragma once

#include <cstdint>
#include <json/value.h>
#include <optional>
#include <shared/validation/validation_dsl.hxx>
#include <string>

struct UpdateCalendarEventDto
{
  std::optional<std::string> title;
  std::optional<std::string> description;
  std::optional<std::string> location;
  std::optional<std::string> color;
  std::optional<int64_t> startsAt;
  std::optional<int64_t> endsAt;
  std::optional<bool> isAllDay;
  std::optional<std::string> recurrenceRule;
  std::optional<int64_t> projectId;

  static UpdateCalendarEventDto fromJson(const Json::Value& json);
};
