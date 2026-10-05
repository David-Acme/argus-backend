#pragma once

#include <json/value.h>

struct UpdateSafetyDto
{
  bool duressEnabled{false};

  static UpdateSafetyDto fromJson(const Json::Value& json);
};
