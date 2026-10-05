#pragma once

#include <json/value.h>
#include <optional>
#include <string>

struct SetPinDto
{
  std::string disarmPin;
  std::string duressPin;
  std::optional<std::string> currentPin;

  static SetPinDto fromJson(const Json::Value& json);
};
