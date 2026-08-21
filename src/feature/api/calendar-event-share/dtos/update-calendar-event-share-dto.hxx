#pragma once

#include <json/value.h>
#include <shared/validation/validation_dsl.hxx>
#include <string>

struct UpdateCalendarEventShareDto
{
  std::string access;

  static UpdateCalendarEventShareDto fromJson(const Json::Value& json);
};
