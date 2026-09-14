#pragma once

#include <json/value.h>
#include <string>

// Owner administrative mode change: {"mode": "home|away|night|armed"}.
struct UpdateGuardModeDto
{
  std::string mode;

  static UpdateGuardModeDto fromJson(const Json::Value& json);
};
