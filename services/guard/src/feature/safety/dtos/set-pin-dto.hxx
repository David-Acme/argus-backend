#pragma once

#include <json/value.h>
#include <string>

struct SetPinDto
{
  std::string disarmPin;
  std::string duressPin;

  static SetPinDto fromJson(const Json::Value& json);
};
