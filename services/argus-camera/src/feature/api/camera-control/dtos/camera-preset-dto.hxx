#pragma once

#include <json/value.h>
#include <shared/validation/validation_dsl.hxx>
#include <string>

// /camera/{id}/preset body: action is `goto`, `save` or `delete`.
struct CameraPresetDto
{
  std::string action;
  std::string id;
  std::string name;

  static CameraPresetDto fromJson(const Json::Value& json);
};
