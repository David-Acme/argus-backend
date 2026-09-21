#pragma once

#include <json/value.h>
#include <validation/validation_dsl.hxx>
#include <string>

struct UpdateCalendarEventShareDto
{
  std::string access;

  static UpdateCalendarEventShareDto fromJson(const Json::Value& json);
};
