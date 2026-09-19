#pragma once

#include <cstdint>
#include <json/value.h>
#include <optional>
#include <shared/validation/validation_dsl.hxx>
#include <string>

struct CreateCalendarEventDto
{
  std::string title;
  std::string description;
  std::string location;
  std::string color;
  int64_t startsAt{0};
  std::optional<int64_t> endsAt;
  bool isAllDay{false};
  std::optional<std::string> recurrenceRule;
  std::optional<int64_t> projectId;

  static CreateCalendarEventDto fromJson(const Json::Value& json);
};
