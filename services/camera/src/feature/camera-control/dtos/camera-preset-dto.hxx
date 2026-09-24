#pragma once

#include <json/value.h>
#include <validation/validation_dsl.hxx>
#include <string>

struct CameraPresetDto
{
  std::string action;
  std::string id;
  std::string name;

  static CameraPresetDto fromJson(const Json::Value& json);
};
