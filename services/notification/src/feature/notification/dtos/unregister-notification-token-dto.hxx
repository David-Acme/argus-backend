#pragma once

#include <json/value.h>
#include <validation/validation_dsl.hxx>
#include <string>

struct UnregisterNotificationTokenDto
{
  std::string token;

  static UnregisterNotificationTokenDto fromJson(const Json::Value& json);
};
