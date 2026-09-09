#pragma once

#include <json/value.h>
#include <shared/validation/validation_dsl.hxx>
#include <string>

struct RegisterNotificationTokenDto
{
  std::string token;
  std::string platform;
  std::string lang;

  static RegisterNotificationTokenDto fromJson(const Json::Value& json);
};
