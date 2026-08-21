#pragma once

#include <cstdint>
#include <json/value.h>
#include <shared/validation/validation_dsl.hxx>
#include <string>

struct CreateCalendarEventShareDto
{
  int64_t calendarEventId{0};
  int64_t userId{0};
  std::string access;

  static CreateCalendarEventShareDto fromJson(const Json::Value& json);
};
