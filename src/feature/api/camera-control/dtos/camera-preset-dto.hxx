#pragma once

#include <json/value.h>
#include <shared/validation/validation_dsl.hxx>
#include <string>

struct CameraPresetDto
{
  /** `goto`, `save` or `delete`. */
  std::string action;
  std::string id;
  std::string name;

  static CameraPresetDto fromJson(const Json::Value& json);
};
