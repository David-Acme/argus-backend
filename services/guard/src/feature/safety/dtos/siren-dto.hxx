#pragma once

#include <cstdint>
#include <json/value.h>

struct SirenDto
{
  int64_t cameraId{0};
  bool enabled{false};

  static SirenDto fromJson(const Json::Value& json);
};
