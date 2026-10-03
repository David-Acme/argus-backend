#pragma once

#include <json/value.h>
#include <string>

struct UpdateCameraContextDto
{
  std::string role;
  bool outdoor{false};
  bool publicArea{false};
  std::string activeHours;

  static UpdateCameraContextDto fromJson(const Json::Value& json);
};
