#pragma once

#include <cstdint>
#include <json/value.h>
#include <optional>
#include <string>

struct UpdateCameraContextDto
{
  std::string role;
  bool outdoor{false};
  bool publicArea{false};
  std::string activeHours;
  std::optional<int64_t> environmentId;

  static UpdateCameraContextDto fromJson(const Json::Value& json);
};
