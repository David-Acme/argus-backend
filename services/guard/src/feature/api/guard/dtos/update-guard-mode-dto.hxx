#pragma once

#include <json/value.h>
#include <string>

struct UpdateGuardModeDto
{
  std::string mode;

  static UpdateGuardModeDto fromJson(const Json::Value& json);
};
