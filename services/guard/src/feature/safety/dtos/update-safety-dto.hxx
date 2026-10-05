#pragma once

#include <json/value.h>
#include <optional>
#include <string>

struct UpdateSafetyDto
{
  bool duressEnabled{false};
  std::optional<std::string> currentPin;

  static UpdateSafetyDto fromJson(const Json::Value& json);
};
