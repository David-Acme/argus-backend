#pragma once

#include <cstdint>
#include <json/value.h>

struct MaintenanceViewDto
{
  int64_t cameraId{0};
  bool accept{false};
  bool typed{true};

  static MaintenanceViewDto fromJson(const Json::Value& json);
};
