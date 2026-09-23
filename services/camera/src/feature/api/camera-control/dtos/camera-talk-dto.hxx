#pragma once

#include <json/value.h>
#include <validation/validation_dsl.hxx>
#include <string>

struct CameraTalkDto
{
  std::string text;
  std::string lang;

  static CameraTalkDto fromJson(const Json::Value& json);
};
