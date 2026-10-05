#pragma once

#include <cstdint>
#include <json/value.h>
#include <optional>
#include <string>

struct UpdateGuardModeDto
{
  std::string mode;
  std::optional<int64_t> environmentId;
  std::optional<std::string> pin;

  static UpdateGuardModeDto fromJson(const Json::Value& json);
};
