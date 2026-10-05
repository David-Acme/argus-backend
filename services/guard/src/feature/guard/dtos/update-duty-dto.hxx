#pragma once

#include <json/value.h>

struct UpdateDutyDto
{
  bool onDuty{false};
  bool typed{true};

  static UpdateDutyDto fromJson(const Json::Value& json);
};
